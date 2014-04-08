#include "param.h"
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"
#include "fcntl.h"
#include "syscall.h"
#include "traps.h"
#include "memlayout.h"

int
main(int argc, char *argv[])
{
	printf(1,"Hello world\n");

	int i = 0;
/*
	while(i < 30000){
			if(i%1000==0 || i > 28000)
				printf(1,"%d pages\n",i);
			i++;
			malloc(4096);
	}
*/
	while(i<28133){
		malloc(4096);
		i++;
	}
	printf(1,"Memory successfully filled\n");
	printf(1,"Next malloc should cause swaps\n");

	malloc(4096);
	printf(1,"Exitting\n");
	exit(); //Everything is free en masse once exit is called
}
