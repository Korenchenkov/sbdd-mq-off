#include <stdio.h>
#include <linux/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>


/* IOCTL */
#define SBDD_DO_IT _IOW( 0xad, 0, char * )

#define SBDD_NAME "sbdd"
#define SBDD_NAME_0 SBDD_NAME "0"

int main(int argc, char *argv[])
{
	int sbdd;

    if (argc < 2)
    {
        printf("Usage:\n\t%s /dev/<raw dev name>\n", argv[0]);
        return 1;
    }
   
    if ((sbdd = open("/dev/" SBDD_NAME_0, O_RDWR)) < 0)
    {
        printf("error: open /dev/%s: %m", SBDD_NAME_0);
        return sbdd;
    }    

    printf("do it... <%s>\n", argv[1]);
    if (ioctl(sbdd, SBDD_DO_IT, argv[1]) < 0)
    {
        fprintf(stderr, "Kernel call returned: %m");
        return 1;
    }
    printf("OK\n");

    return 0;
}
