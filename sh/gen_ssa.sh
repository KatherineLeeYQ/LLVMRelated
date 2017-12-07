sudo make
clang -emit-llvm -S -g3 ./test/test20.c -o ./outfiles/test20.bc
./bin/assignment ./outfiles/test20.bc 2> ./real_ssa/test20.txt
