/*
 * (C) 2018 - ntop
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#define _GNU_SOURCE
#include <signal.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <poll.h>
#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <net/ethernet.h>     /* the L2 protocols */
#include <sys/time.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <monetary.h>
#include <locale.h>

#include "pfring.h"
#include "pfring_ft.h"

#include "pfutils.c"

#define DEFAULT_DEVICE     "eth0"

pfring *pd = NULL;
pfring_ft_table *ft = NULL;
int bind_core = -1;
u_int8_t quiet = 0, verbose = 0, do_shutdown = 0;

/* ************************************ */

static char *_intoa(unsigned int addr, char* buf, u_short bufLen) {
  char *cp, *retStr;
  u_int byte;
  int n;

  cp = &buf[bufLen];
  *--cp = '\0';

  n = 4;
  do {
    byte = addr & 0xff;
    *--cp = byte % 10 + '0';
    byte /= 10;
    if (byte > 0) {
      *--cp = byte % 10 + '0';
      byte /= 10;
      if (byte > 0)
	*--cp = byte + '0';
    }
    *--cp = '.';
    addr >>= 8;
  } while (--n > 0);

  retStr = (char*)(cp+1);

  return (retStr);
}

/* ************************************ */

void sigproc(int sig) {
  static int called = 0;

  fprintf(stderr, "Leaving...\n");
  if (called) return; else called = 1;

  do_shutdown = 1;
  pfring_breakloop(pd);
}

/* ******************************** */

void processFlow(pfring_ft_flow *flow, void *user){
  pfring_ft_flow_key *k;
  pfring_ft_flow_value *v;
  char buf1[32], buf2[32], buf3[32];
  char *ip1, *ip2;

  k = pfring_ft_flow_get_key(flow);
  v = pfring_ft_flow_get_value(flow);

  if (k->ip_version == 4){
    ip1 = _intoa(k->saddr.v4, buf1, sizeof(buf1));
    ip2 = _intoa(k->daddr.v4, buf2, sizeof(buf2));
  } else {
    ip1 = (char *) inet_ntop(AF_INET6, &k->saddr.v6, buf1, sizeof(buf1));
    ip2 = (char *) inet_ntop(AF_INET6, &k->daddr.v6, buf2, sizeof(buf2));
  }

  printf("[Flow] "
         "srcIp: %s, dstIp: %s, srcPort: %u, dstPort: %u, protocol: %u, tcpFlags: 0x%02X, "
         "l7: %s, "
         "c2s: { Packets: %ju, Bytes: %ju, First: %u.%u, Last: %u.%u }, "
         "s2c: { Packets: %ju, Bytes: %ju, First: %u.%u, Last: %u.%u }\n",
         ip1, ip2, k->sport, k->dport, k->protocol, v->tcp_flags[s2d_direction] | v->tcp_flags[d2s_direction],
         pfring_ft_l7_protocol_name(ft, &v->l7_protocol, buf3, sizeof(buf3)),
         v->pkts[s2d_direction], v->bytes[s2d_direction], 
         (u_int) v->first[s2d_direction].tv_sec, (u_int) v->first[s2d_direction].tv_usec, 
         (u_int) v->last[s2d_direction].tv_sec,  (u_int) v->last[s2d_direction].tv_usec,
         v->pkts[d2s_direction], v->bytes[d2s_direction], 
         (u_int) v->first[d2s_direction].tv_sec, (u_int) v->first[d2s_direction].tv_usec, 
         (u_int) v->last[d2s_direction].tv_sec,  (u_int) v->last[d2s_direction].tv_usec);
}

/* ******************************** */

void processPacket(const struct pfring_pkthdr *h,
		   const u_char *p, const u_char *user_bytes) {
  char buffer[256];
  pfring_ft_action action;

  action = pfring_ft_process(ft, p, (pfring_ft_pcap_pkthdr *) h);

  if (verbose) {
    buffer[0] = '\0';
    pfring_print_pkt(buffer, sizeof(buffer), p, h->len, h->caplen);
    printf("[Packet]%s %s", action == PFRING_FT_ACTION_DISCARD ? " [discard]" : "", buffer);
  }
}

/* ******************************** */

void packet_consumer() {
  struct pfring_pkthdr hdr;
  u_char *buffer_p = NULL;

  memset(&hdr, 0, sizeof(hdr));

  while (!do_shutdown) {
    if (pfring_recv(pd, &buffer_p, 0, &hdr, 0) > 0) {
      processPacket(&hdr, buffer_p, NULL);
    } else {
      if (!pfring_ft_housekeeping(ft, time(NULL))) {
        usleep(1);
      }
    }
  }
}

/* *************************************** */

void printHelp(void) {
  printf("pfflow_ft - (C) 2018 ntop.org\n\n");
  printf("-h              Print this help\n");
  printf("-i <device>     Device name. Use:\n");
  printf("-q              Quiet mode\n");
  printf("-v              Verbose (print also raw packets)\n");
}

/* *************************************** */

int main(int argc, char* argv[]) {
  char *device = NULL, c;
  int promisc, snaplen = 1518, rc;
  u_int32_t flags = 0;
  packet_direction direction = rx_and_tx_direction;

  while ((c = getopt(argc,argv,"g:hi:qv")) != '?') {
    if ((c == 255) || (c == -1)) break;

    switch(c) {
    case 'g':
      bind_core = atoi(optarg);
      break;
    case 'h':
      printHelp();
      exit(0);
      break;
    case 'i':
      device = strdup(optarg);
      break;
    case 'q':
      quiet = 1;
      break;
    case 'v':
      verbose = 1;
      break;
    }
  }

  if (device == NULL) device = DEFAULT_DEVICE;
  bind2node(bind_core);

  ft = pfring_ft_create_table(PFRING_FT_TABLE_FLAGS_DPI);

  if (ft == NULL) {
    fprintf(stderr, "pfring_ft_create_table error\n");
    return -1;
  }

  /* Example of L7 packet filtering/shunting loading from configuration file
  pfring_ft_load_configuration(ft, "rules.conf");
  */

  /* Example of L7 packet filtering rules
  pfring_ft_set_filter_protocol_by_name(ft, "MDNS", PFRING_FT_ACTION_DISCARD);
  pfring_ft_set_filter_protocol_by_name(ft, "UPnP", PFRING_FT_ACTION_DISCARD);
  */

  pfring_ft_set_export_callback_flow(ft, processFlow, NULL);

  promisc = 1;

  if (promisc) flags |= PF_RING_PROMISC;
  flags |= PF_RING_TIMESTAMP; /* needed for flow processing (FIXX optimise ts generation) */

  pd = pfring_open(device, snaplen, flags);

  if (pd == NULL) {
    fprintf(stderr, "pfring_open error [%s] (pf_ring not loaded or interface %s is down ?)\n",
      strerror(errno), device);
    return -1;
  } else {
    u_int32_t version;

    pfring_set_application_name(pd, "pfflow_ft");
    pfring_version(pd, &version);

    if (!quiet) {
      printf("Using PF_RING v.%d.%d.%d\n",
       (version & 0xFFFF0000) >> 16,
       (version & 0x0000FF00) >> 8,
       version & 0x000000FF); 
    }
  }

  pfring_set_direction(pd, direction);

  if ((rc = pfring_set_socket_mode(pd, recv_only_mode)) != 0)
    fprintf(stderr, "pfring_set_socket_mode returned [rc=%d]\n", rc);

  signal(SIGINT, sigproc);
  signal(SIGTERM, sigproc);

 if (pfring_enable_ring(pd) != 0) {
    printf("Unable to enable ring :-(\n");
    pfring_close(pd);
    return -1;
  }

  if (bind_core >= 0)
    bind2core(bind_core);

  packet_consumer();

  sleep(1);

  pfring_close(pd);

  pfring_ft_flush(ft);

  pfring_ft_destroy_table(ft);

  return 0;
}
