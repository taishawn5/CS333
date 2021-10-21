#ifdef CS333_P2
#include "types.h"
#include "user.h"
#include "uproc.h"

int
main(int argc, char * argv[])
{
  int max = atoi(argv[1]);
  if(max == NULL)
    max = NPROC;
  struct uproc * table = (struct uproc*) malloc(max * sizeof(struct uproc));
  int process = getprocs(max,table);

  if(process < 0)
    printf(2,"There is nothing to display.");
  printf(1,"\n\nPID\tName\tUID\tGID\tPPID\tElapsed\tCPU\tState\tSize\t\n");
  for(int i=0;i<process;++i)
  {
    printf(1,"%d\t%s\t%d\t%d\t%d\t%d.%d\t%d.%d\t%s\t%d\t\n",
      table[i].pid, table[i].name, table[i].uid, table[i].gid, table[i].ppid, table[i].elapsed_ticks/1000, table[i].elapsed_ticks % 1000,
      table[i].CPU_total_ticks/1000, table[i].CPU_total_ticks % 1000, table[i].state, table[i].size);
  }
  exit();
}
#endif



