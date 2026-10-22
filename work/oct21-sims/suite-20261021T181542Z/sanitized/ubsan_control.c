#include <limits.h>
#include <stdio.h>
int main(int argc, char **argv) { int x = INT_MAX - 1 + argc; (void)argv; x += 1; printf("%d\n", x); return 0; }
