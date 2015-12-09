#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#define ENV_HOME "HOME"
#define ENV_SHELL "SHELL"
#define ENV_EDITOR "EDITOR"
#define ENV_DIFFPROG "DIFFPROG"

#define ACT_BACKUP 0
#define ACT_EDIT 1
#define ACT_HELP 2
#define ACT_ERROR 3

#define MIN(X, Y) ((X) < (Y) ? (X) : (Y))
#define MAX(X, Y) ((X) < (Y) ? (Y) : (X))
#define MAX_PATH_LN 256
#define BACKUP "backup"

static char *home_dir;
static char backup_tar_gz[MAX_PATH_LN];
static char working_dir[MAX_PATH_LN];
static char backup_dir[MAX_PATH_LN];
static char backup_conf[MAX_PATH_LN];

static int error(char *fmt, ...)
{
#define MAX_ERR_LN 256
#define ERR_PREFIX "error: "
#define ERR_SUFFIX "\n"
    va_list ap;
    char err_str[MAX_ERR_LN];

    memset(err_str, 0, MAX_ERR_LN);
    va_start(ap, fmt);
    vsnprintf(err_str, MAX_ERR_LN, fmt, ap);
    va_end(ap);

    return fprintf(stderr, "%s%s%s", ERR_PREFIX, err_str, ERR_SUFFIX);
}

/* removes a newline if it comes as the last non zero charcater in str
 * return: 0 - if the string was not modified
 *         1 - if the string was modified */
static int remove_newline(char *str)
{
    char *c = str + strlen(str) - 1;

    if (*c == '\n')
    {
	*c = 0;
	return 1;
    }
    return 0;
}

/* adds a newline at the end of str if a the resulting string will still be null
 * terminated.
 * return: 0 - if the string was not modified
 *         1 - if the string was modified */
static int add_newline(char *str)
{
    char *c = str + strlen(str);

    if (*(c + 1) != 0)
	return 0;
    *c = '\n';
    return 1;
}

static int del_obsolete_entry(char *path)
{
#define YES "yes"
#define NO "no"
#define NEWLINE '\n'
#define INPUT_SZ 5 /* strlen(MAX(YES, NO)) + strlen("\n") + 1(null terminator) */

    char ans[INPUT_SZ];
    int ret = 0, modified = 0;

    printf("%s does not exist.\n", path);
    printf("Do you want to remove it from the configuration file? [Y/n] ");
    bzero(ans, INPUT_SZ);
    fgets(ans, INPUT_SZ, stdin);
    modified = remove_newline(ans);

    while (!((MAX(strlen(ans), strlen(YES)) == strlen(YES)) &&
	(!strncasecmp(ans, YES, MIN(strlen(ans), strlen(YES))))) &&
	!((MAX(strlen(ans), strlen(NO)) == strlen(NO)) &&
	 !strncasecmp(ans, NO, MIN(strlen(ans), strlen(NO)))))
    {
	/* if a NEWLINE was not encountered, it must be removed from stdin */
	if (!modified)
	    while(fgetc(stdin) != NEWLINE);
	bzero(ans, INPUT_SZ);
	printf("Please answer Y[es] or n[o]: ");
	fgets(ans, INPUT_SZ, stdin);
	modified = remove_newline(ans);
    }

    switch (*ans)
    {
	case 0: /* a NEWLINE was replaced by the null terminator */
	case 'y':
	case 'Y':
	    ret = 1;
	    break;
	case 'n':
	case 'N':
	    ret = 0;
	    break;
	default:
	    /* TODO: sanity check */
	    break;
    }
    return ret;
}

static int cp_file(FILE *to, FILE *from)
{
    char line[MAX_PATH_LN];

    bzero(line, MAX_PATH_LN);
    while (fgets(line, MAX_PATH_LN, from))
    {
	fputs(line, to);
	bzero(line, MAX_PATH_LN);
    }
    /* TODO */
    return fseek(to, 0, SEEK_SET);
}

static int sys_exec(char *format, ...)
{
    va_list ap;
    int ret = 0, status;
    char *args[4];

    char command[MAX_PATH_LN];

    bzero(command, MAX_PATH_LN);
    va_start(ap, format);
    ret = vsnprintf(command, MAX_PATH_LN - 1, format, ap);
    va_end(ap);

    if (ret < 0)
	return -1;

    if (!fork()) /* child process */
    {
	args[0] = getenv(ENV_SHELL);
	args[1] = "-c";
	args[2] = command;
	args[3] = NULL;
	execvp(args[0], args);
    }
    wait(&status);
    if (ret < 0)
	return -1;

    return 0;
}

static void cleanup(void)
{
}

static int init(void)
{
    if (!(home_dir = getenv(ENV_HOME)))
    {
	error("getenv()");
	return -1;
    }
    if (!getcwd(working_dir, MAX_PATH_LN))
    {
	error("getcwd()");
	return -1;
    }
    if ((snprintf(backup_tar_gz, MAX_PATH_LN, "%s/backup.tar.gz", 
	working_dir)) < 0)
    {
	error("snprintf()");
	return -1;
    }

    if ((snprintf(backup_conf, MAX_PATH_LN, "%s/.backup.conf", home_dir) < 0))
    {
	return -1;
    }
    return 0;
}

static int create_backup_dir(void)
{
    int ret, i = 0;
    char *tmp = backup_dir;

    printf("backing up...\n");
    bzero(backup_dir, MAX_PATH_LN);
    bzero(tmp, MAX_PATH_LN);
    /* add leading underscores if the backup directory allready exists */
    do
    {
	snprintf(&(tmp[i]), MAX_PATH_LN - (i + i), BACKUP);
	if ((ret = mkdir(backup_dir, 0777)) == -1)
	{
	    if (errno != EEXIST)
	    {
		error("mkdir()");
		return -1;
	    }

	    tmp[i] = '_';
	}
	i++;
    }
    while (ret && (i < (MAX_PATH_LN - strlen(backup_conf) - 1)));

    return ret;
}

static int cp_to_budir(void)
{
    FILE *tmp = NULL, *cnf = NULL;
    char path[MAX_PATH_LN];
    struct stat buf;

    /* open cnf FILE */
    if (!(cnf = fopen(backup_conf, "r+")))
    {
	error("fopen(%s, \"r+\")", backup_conf);
	return -1;
    }

    /* open tmp FILE */
    if (!(tmp = tmpfile()))
    {
	error("tmpfile()");
	return -1;
    }

    /* copy conf file to tmp file and create a new conf file */
    cp_file(tmp, cnf);
    fclose(cnf);
    if (remove(backup_conf))
    {
	error("remove()");
	fclose(tmp);
	return -1;
    }
    if (!(cnf = fopen(backup_conf, "w+")))
    {
	error("fopen(%s, \"w+\")", backup_conf);
	fclose(tmp);
	return -1;
    }

    /* copy backup destinations to backup_dir and create new conf file */
    bzero(path, MAX_PATH_LN);
    while (fgets(path, MAX_PATH_LN, tmp))
    {
	remove_newline(path);
	if (stat(path, &buf) == -1)
	{
	    if (errno != ENOENT)
	    {
		error("stat(%s, &buf), continuing...", path);
		bzero(path, MAX_PATH_LN);
		continue;
	    }
	    /* copy a non statted (non existant) path to new conf */
	    if (!del_obsolete_entry(path))
	    {
		add_newline(path);
		fputs(path, cnf);
		bzero(path, MAX_PATH_LN);
		continue;
	    }

	    bzero(path, MAX_PATH_LN);
	    continue;
	}

	if (sys_exec("cp -r --parents %s %s", path, backup_dir))
	{
	    cleanup();
	    return -1;
	}
	add_newline(path);
	fputs(path, cnf);
	bzero(path, MAX_PATH_LN);
    }

    fclose(tmp);
    fclose(cnf);
    return 0;
}

static int make_tar_gz(void)
{
    if (sys_exec("cd %s && tar czf %s .", backup_dir, backup_tar_gz))
	return -1;
    return 0;
}

static int remove_backup_dir(void)
{
    if (sys_exec("rm -rf %s", backup_dir))
	return -1;
    printf("done: %s\n", backup_tar_gz);
    return 0;
}

/*TODO*/
static int get_args(int argc, char *argv[])
{
#define ARG_EL "beh" /* TODO: v(verbose), h(help) */

    int ret;

    if (argc != 2)
	return ACT_ERROR;

    switch ((char)getopt(argc, argv, ARG_EL))
    {
	case 'b':
	    ret = ACT_BACKUP;
	    break;
	case 'e':
	    ret = ACT_EDIT;
	    break;
	case 'h':
	    ret = ACT_HELP;
	    break;
	default:
	    ret = ACT_ERROR;
	    break;
    }
    return ret;
}

static int backup(void)
{
    create_backup_dir();
    cp_to_budir();
    make_tar_gz();
    remove_backup_dir();

    return 0;
}

/*TODO*/
static int edit(void)
{
#define DEFAULT_EDITOR "vim"
#define DEFAULT_DIFFPROG "diff"
#define OPEN_NEW "w+"
#define OPEN_EXIST "r+"

    char *editor = DEFAULT_EDITOR;
    char *diffprog = DEFAULT_DIFFPROG;
    char *env_var = NULL;
    char mode[3] = OPEN_EXIST, backup_conf_tmp[MAX_PATH_LN];
    FILE *tmp, *cnf;
    struct stat buf;
    int new_file = 0;

    bzero(backup_conf_tmp, MAX_PATH_LN);
    snprintf(backup_conf_tmp, MAX_PATH_LN, "%s.tmp", backup_conf);

    /* test if conf file exists, if not it must be created */
    if (stat(backup_conf, &buf) == -1)
    {
	snprintf(mode, 3, OPEN_NEW);
	new_file = 1;
    }

    if (!(tmp = fopen(backup_conf_tmp, "w+")) || 
	!(cnf = fopen(backup_conf, mode)))
    {
	return -1;
    }

    cp_file(tmp, cnf);

    if ((env_var = getenv(ENV_EDITOR)))
	editor = env_var;
    if ((env_var = getenv(ENV_DIFFPROG)))
	diffprog = env_var;

    sys_exec("%s %s", editor, backup_conf);
    if (!new_file)
	sys_exec("%s %s %s", diffprog, backup_conf, backup_conf_tmp);
    fclose(tmp);
    fclose(cnf);
    remove(backup_conf_tmp);
    return 0;
}

static void usage(void)
{
#define FMT_HIGHLIGHT "\033[1;38m"
#define FMT_UNDERLINE "\033[4;38m"
#define FMT_RESET "\033[00;00;00m"
#define COPYRIGHT 0xA9

    printf(
	"usage:	%sbackup < -e | -b | -h >%s\n"
	"   where\n"
	"   %s-e%s  Edit the configuration file.\n"
	"	Set the environment variable %sEDITOR%s to use an editor of "
	"your\n"
	"	choice. If %sEDITOR%s is not set, %sbackup%s will use %svim%s "
	"for editing\n"
	"	the configuration file.\n"
	"	Once an existing configuration file is modifed, %sbackup%s\n"
	"	displays a diff between the previous and the current "
	"versions.\n"
	"	Set the environment variable %sDIFFPROG%s to use a diff "
	"program\n"
	"	of your choice. If %sDIFFPROG%s is not set, %sbackup%s will "
	"use %sGNU \n"
	"	diff%s.\n"
	"   %s-b%s  Backup the files and directories mentioned in the \n"
	"	configuration file. The gzipped tarball %sbackup.tar.gz%s\n"
	"	containing the backed up files will be placed in the \n"
	"	working directory.\n"
	"   %s-h%s  Print this message and exit.\n"
	"\n%s%c IAS Software, October 2004%s\n",
	FMT_HIGHLIGHT, FMT_RESET,
	FMT_HIGHLIGHT, FMT_RESET,
	FMT_UNDERLINE, FMT_RESET,
	FMT_UNDERLINE, FMT_RESET, FMT_HIGHLIGHT, FMT_RESET, FMT_HIGHLIGHT, 
	FMT_RESET,
	FMT_HIGHLIGHT, FMT_RESET,
	FMT_UNDERLINE, FMT_RESET,
	FMT_UNDERLINE, FMT_RESET, FMT_HIGHLIGHT, FMT_RESET,
	FMT_HIGHLIGHT, FMT_RESET,
	FMT_HIGHLIGHT, FMT_RESET,
	FMT_HIGHLIGHT, FMT_RESET,
	FMT_HIGHLIGHT, FMT_RESET,
	FMT_HIGHLIGHT, COPYRIGHT, FMT_RESET);
}

int main(int argc, char *argv[])
{
    int ret = 0;

    init();
    switch (get_args(argc, argv))
    {
	case ACT_BACKUP:
	    ret = backup();
	    break;
	case ACT_EDIT:
	    ret = edit();
	    break;
	case ACT_HELP:
	    usage();
	    break;
	default:
	    error("try 'backup -h' for more information");
	    break;
    }
    return ret;
}
