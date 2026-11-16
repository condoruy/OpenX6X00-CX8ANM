mbelib 1.3.0 from https://github.com/szechyjs/mbelib (commit 9a04ed5c78176a9965f3d43f7aa1b1f5330e771f Tue May 28 21:43:53 2019 -0400).
Only the AMBE 3600x2450 decoder used by DMR is kept: mbelib.c, ambe3600x2450.c, ecc.c
and their headers, unchanged. License: ISC (COPYRIGHT).

mbelib's own README notes that the AMBE+2 algorithm may be covered by patents in some countries;
the radio only decodes (listens) and never encodes.
