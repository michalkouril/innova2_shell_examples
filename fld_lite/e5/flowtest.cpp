// Which ethertype flow rules does mlx5 accept on a raw packet QP? (E5.2 create_flow EINVAL, 2026-09-30)
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
int main(int argc, char **argv)
{
  int n = 0; ibv_device **l = ibv_get_device_list(&n); ibv_context *c = nullptr;
  for (int i = 0; i < n; i++) if (!strcmp(ibv_get_device_name(l[i]), argv[1])) c = ibv_open_device(l[i]);
  ibv_pd *pd = ibv_alloc_pd(c); ibv_cq *cq = ibv_create_cq(c, 64, nullptr, nullptr, 0);
  ibv_qp_init_attr ia{}; ia.send_cq = cq; ia.recv_cq = cq; ia.qp_type = IBV_QPT_RAW_PACKET; ia.cap.max_recv_wr = 16; ia.cap.max_recv_sge = 1; ia.cap.max_send_wr = 1; ia.cap.max_send_sge = 1;
  ibv_qp *qp = ibv_create_qp(pd, &ia);
  ibv_qp_attr a{}; a.qp_state = IBV_QPS_INIT; a.port_num = 1; ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_PORT);
  a = {}; a.qp_state = IBV_QPS_RTR; ibv_modify_qp(qp, &a, IBV_QP_STATE);
  for (int i = 2; i < argc; i++) {
    const unsigned et = strtoul(argv[i], nullptr, 0);
    struct { ibv_flow_attr a; ibv_flow_spec_eth e; } __attribute__((packed)) fl{};
    fl.a.type = IBV_FLOW_ATTR_NORMAL; fl.a.size = sizeof(fl); fl.a.num_of_specs = 1; fl.a.port = 1;
    fl.e.type = IBV_FLOW_SPEC_ETH; fl.e.size = sizeof(fl.e); fl.e.val.ether_type = htons(et); fl.e.mask.ether_type = 0xFFFF;
    ibv_flow *f = ibv_create_flow(qp, &fl.a);
    printf("ethertype 0x%04x: %s\n", et, f ? "OK" : strerror(errno));
    if (f) ibv_destroy_flow(f);
  }
  return 0;
}
