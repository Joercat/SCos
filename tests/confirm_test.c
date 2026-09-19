#include "scos.h"
extern int printf(const char *, ...);
#define CHECK(c) do { if(!(c)) { printf("FAIL confirmation line %d\n",__LINE__); return 1; } } while(0)
int main(void) {
    CHECK(confirm_answer(" Y ")==1 && confirm_answer("YES")==1);
    CHECK(confirm_answer("")==0 && confirm_answer("n")==0 && confirm_answer("No")==0);
    CHECK(confirm_answer("yes please")==-1 && confirm_answer("maybe")==-1);
    struct shell_confirm a={{0}}, b={{0}};
    char line[256]="  kill   --system   2  ", msg[192];
    CHECK(confirm_command(&a,line,sizeof(line),msg)==1);
    CHECK(!b.command[0]);
    strcpy(line,"maybe"); CHECK(confirm_command(&a,line,sizeof(line),msg)==1 && a.command[0]);
    strcpy(line,"n"); CHECK(confirm_command(&a,line,sizeof(line),msg)==1 && !a.command[0]);
    strcpy(line,"kill --system 2"); CHECK(confirm_command(&a,line,sizeof(line),msg)==1);
    strcpy(line,"y"); CHECK(confirm_command(&a,line,sizeof(line),msg)==0 && !strcmp(line,"kill --system 2"));
    CHECK(!a.command[0]);
    strcpy(line,"reboot"); CHECK(confirm_command(&a,line,sizeof(line),msg)==1);
    line[0]=0; CHECK(confirm_command(&a,line,sizeof(line),msg)==1 && !a.command[0]);
    strcpy(line,"shutdown --confirm"); CHECK(confirm_command(&a,line,sizeof(line),msg)==1);
    strcpy(line,"yes"); CHECK(confirm_command(&a,line,sizeof(line),msg)==0);
    strcpy(line,"rm /system/boot -s"); CHECK(confirm_command(&a,line,sizeof(line),msg)==1);
    strcpy(line,"n"); CHECK(confirm_command(&a,line,sizeof(line),msg)==1);
    strcpy(line,"rm -i /home/test"); CHECK(confirm_command(&a,line,sizeof(line),msg)==1);
    strcpy(line,"y"); CHECK(confirm_command(&a,line,sizeof(line),msg)==0);
    int pid; CHECK(parse_pid("2",&pid) && pid==2);
    CHECK(!parse_pid("",&pid) && !parse_pid("oops",&pid) && !parse_pid("-1",&pid));
    CHECK(!parse_pid("9999999999999999999",&pid));
    printf("Shared confirmations: cancellation, exact y/n, shell isolation, whitespace and strict PIDs PASS\n");
    return 0;
}
