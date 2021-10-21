#ifdef CS333_P2
#include "types.h"
#include "user.h"

int
main(int argc, char * argv[])
{
  int start_ticks,final_ticks,run_ticks,pid;
  pid=fork();
  start_ticks=uptime();

  if(pid==0)
    {
      if((exec(argv[1],&argv[1])) <0)
        printf(2,"This function had a problem %s.\n",argv[1]);
    }
  else
  {
    wait();
    final_ticks=uptime();
    run_ticks=final_ticks - start_ticks;
    printf(1,"\n%s finished in %d.",argv[1],run_ticks/1000);
    if(run_ticks%1000 < 10)
      printf(1,"%d00 seconds.\n",run_ticks%1000);
    if(run_ticks%1000 < 100 && run_ticks%1000 >=10)
      printf(1,"%d0 seconds.\n",run_ticks%1000);
    if(run_ticks%1000 >= 100)
      printf(1,"%d seconds.\n",run_ticks%1000);

  }
    exit();
}
#endif
