#include "types.h"
#include "stat.h"
#include "user.h"
#include "syscall.h"
#include "types.h"
#include "user.h"
#include "syscall.h"

#define PGSIZE 4096


int
main(int argc, char *argv[])
{
	#if TASKONE
printf(1, "**********************************TASK 1**********************************\n");

//pmalloc test 
printf(1, "Start alocate pages with pmalloc/malloc...");
int n = 6;
char *parray[n];
int protarray[n];
for (int k = 0; k < n; ++k) {
 if(k%2==0)
 {
     parray[k]=pmalloc();
 }
 else {
     parray[k]=malloc(k*1000);
 }
}

printf(1, "malloc/pmalloc success! %d\n", (int)parray[0]);
printf(1, "protecting pages\n");

for (int k = 0; k < n; ++k) {
   protarray[k]=protect_page(parray[k]);
}

printf(1, "protecting pages successfully\n");
printf(1, "validating pages ..\n");


for (int k = 0; k < n; ++k) {
   if(k%2==0){
   if (protarray[k]){
       printf(1, "index %d valid!\n",k);
   }
   else{
       printf(1, "index %d not valid!\n",k);

   }
   }
   else {
        if (protarray[k]==-1){
             printf(1, "index %d valid!\n",k);
        }
        else{
            printf(1, "index %d not valid!\n",k);

        }
   
   }
   
}
int pid=fork();
if(pid==0){
	printf(1, "We access protected page TRAP 13 should appeare:\n");
	parray[0][1]='z';
}
else{

wait();

printf(1, "validating ended successfully\n");
printf(1, "free pages..\n");


for (int k = 0; k < n; ++k) {
    if(k%2==0){
   pfree(parray[k]);
    }
    else{
    free(parray[k]);

    }
}

printf(1, "free pages ended successfully..\n");
	printf(1, "**********************************END TASK 1**********************************\n");
}

    #elif SCFIFO
    int i;
	//create page array
	char *arr[31];
	char input[10];
	printf(1, "**********************************FIFO**********************************\n");
	gets(input, 10);
	//create 13 pages because there are allready 3 that  create in the comp time 
	for (i = 0; i < 13; ++i) {
		arr[i] = sbrk(PGSIZE);
		printf(1, "added page %d\n",i+4);
	}
	printf(1, "\n\ncheck that RAM is full (16 pages) \n\n");
	gets(input, 5);

	printf(1, "\ncreating 4 pages and 1 protected page... \n");
	arr[13] = sbrk(PGSIZE);
	arr[14] = sbrk(PGSIZE);
	arr[15] = sbrk(PGSIZE);
	arr[16] = sbrk(PGSIZE);
	arr[17] = pmalloc();
	protect_page(arr[17]);
	//page 1 is kernel ,pages 0,2 have been accessed
	printf(1, "\n\ncheck that pages 3,4,5,6,7 are taken out in a fifo matter\n\n");
	gets(input, 10);

	printf(1, "\n\n We accessed pages 9,11 \n\n");

	//access to page 9
	arr[6][1]='o';
	//access to page 11
	arr[8][1]='r';

	printf(1, "we try to access pages 3,4,5,6 so now we will get page fault and pages 8,10,12,13 will be swapped: \n\n");


	arr[0][1] = 'z';
	arr[1][1] = 'o';
	arr[2][1] = 'h';
	arr[3][1] = 'a';

	gets(input, 10);
	
	printf(1,"\n\nforking, please wait...\n\n");

	if (fork() == 0) {
		printf(1, "\ncreated a child process %d\n",getpid());
		printf(1, "check: child pages should be identical to father\n");
		gets(input, 10);

		printf(1, "\n\n try to access page 7 ,but it is in the DISC so now we will get page fault in child process Should swap page 14 \n\n");
		arr[4][1] = 'x';
		gets(input, 10);
		exit();
	}
	else {
		wait();
		printf(1, "\n\nfree the protect page\n\n");
		pfree(arr[17]);
		sbrk(-16 * PGSIZE);
		printf(1, "\n\ncheck: father has only 5 pages after deallocation\n\n");
		gets(input, 10);
    	printf(1, "END OF TEST\n");

	}


    #elif LIFO
    int i;
	char *arr[31];
	char input[10];
	printf(1, "**********************************LIFO**********************************\n");
	gets(input, 10);

	for (i = 0; i < 13; ++i) {
		arr[i] = sbrk(PGSIZE);
		printf(1, "added page %d\n",i+4);
	}
    
	printf(1, "\n\ncheck that RAM space is full \n\n");
	gets(input, 5);

	printf(1, "\ncreating 3 pages and 2 protected pages... \n");
	arr[13] = sbrk(PGSIZE);
	arr[14] = sbrk(PGSIZE);
	arr[15] = sbrk(PGSIZE);
		
	arr[16] = pmalloc();
	protect_page(arr[16]);

	arr[17] = pmalloc();
	protect_page(arr[17]);
	printf(1, "\n\ncheck that pages 15,14,13,12,11 are taken out in a Lifo matter\n\n");
	gets(input, 10);
	
	printf(1, "\n\nWe accessed pages 15,14 -the 2 pages we just moved to DISK. check for 2 page faults : ");
	printf(1, "pages 10,9 should be swpped\n\n");
	//accessing to pages 15,14
	int j;
	for (j=12; j>10; j--){ 
		arr[j][1]='Z';
	}

	gets(input, 10);
	
	printf(1,"\n\nforking, please wait...\n\n");

	if (fork() == 0) {
	
		printf(1, "\ncreated a child process %d\n",getpid());
		printf(1, "check: child pages should be identical to father\n");
		gets(input, 10);
		printf(1, "\n\ncheck: 1 page fault in child process (accessed page  which was just moved to DISK) - Should swap page 10 \n\n");
		arr[10][0] = 'x';
		gets(input, 10);
		exit();
	}
	else {
		wait();
		printf(1, "\n\nfree the protect pages\n\n");

		pfree(arr[16]);
		pfree(arr[17]);

		sbrk(-16 * PGSIZE);
		printf(1, "\n\ncheck: father has only 5 pages after deallocation\n\n");
		gets(input, 10);
    	printf(1, "END OF TEST\n");

	}

	#else //NONE
    char* pagesArr[50];
    int i = 50;
    printf(1, "None: no page faults should occur\n");
    for (i = 0; i < 50; i++) {
        pagesArr[i] = sbrk(PGSIZE);
        printf(1, "pagesArr[%d]=0x%x\n", i, pagesArr[i]);
    }
    printf(1, "TEST IS DONE\n");
	#endif
    exit();
}

