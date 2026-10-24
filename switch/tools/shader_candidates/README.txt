How switch/shader_candidates.h was made (needs your own US v1.0 ROM as baserom.z64 in the repository root and the disasm/ checkout (pret/pokestadium) from `git submodule update --init disasm`):
  1. gcc -O2 -o scan scan.c && ./scan baserom.z64 > hits.txt     decompresses every Yay0 block, lists plausible G_SETCOMBINE words
  2. python3 extract.py                                          finds every gDPSetCombine*/gsDPSetCombine* macro in the decomp + decomp source
                                                                 writes gen.c (macros -> words); build it with
                                                                 gcc -w -D_LANGUAGE_C -I lib/ultralib/include -I include -o gen gen.c  (in the pret tree)
                                                                 and run: ./gen > src_words.txt
  3. python3 rank.py                                             scores the words and writes shader_candidates.h
Check: 41 of the 45 distinct combine words the game really built in a Gym Leader Castle run (build 41/44 logs) are in the output.
