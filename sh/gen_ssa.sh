sudo make
clang -emit-llvm -S -g3 ./test/test.c -o ./outfiles/test.bc
./bin/assignment ./outfiles/test.bc 2> ./real_ssa/test.txt
