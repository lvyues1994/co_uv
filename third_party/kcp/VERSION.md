# KCP provenance

Source: https://github.com/skywind3000/kcp
Commit: b1a7a2101dcbb96017681a500d6b82bbe5a88766 (V2)

ikcp.c and ikcp.h are byte-for-byte upstream originals; see LICENSE for the upstream MIT notice.
SHA256 is verified by tests/kcp_vendor_tests.cmake.
No allocator replacement or congestion-control plugins are enabled by uvco.
Some upstream allocation failures assert or dereference null instead of reporting an error.
This approved limitation means Result cannot promise recovery from every vendor OOM.
Never install a process-global allocator while any KcpCore is alive.
