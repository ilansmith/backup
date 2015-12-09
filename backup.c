#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <stdarg.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#define DEFAULT_EDITOR "vim"
#define DEFAULT_DIFFPROG "diff"

#define ENV_HOME "HOME"
#define ENV_SHELL "SHELL"
#define ENV_EDITOR "EDITOR"
#define ENV_DIFFPROG "DIFFPROG"

#define STR_YES "yes"
#define STR_NO "no"

#define COLOR_HIGHLIGHT "\033[1m"
#define COLOR_UNDERLINE "\033[4m"
#define COLOR_ERROR "\033[1;31m" /* bright red */
#define COLOR_RESET "\033[00;00m"

#define TEXT_HIGHLIGHT(_str_) COLOR_HIGHLIGHT _str_ COLOR_RESET
#define TEXT_UNDERLINE(_str_) COLOR_UNDERLINE _str_ COLOR_RESET
#define TEXT_ERROR(_str_) COLOR_ERROR _str_ COLOR_RESET

#define MIN(_x_, _y_) ((_x_) < (_y_) ? (_x_) : (_y_))
#define MAX(_x_, _y_) ((_x_) < (_y_) ? (_y_) : (_x_))
#define MAX_PATH_LEN 256

#define CHAR_SP 0x20
#define CHAR_TAB 0x9 
#define CHAR_NL 0xA
#define CHAR_HS 0X23

#define IS_NEWLINE(_x_) ((_x_) == CHAR_NL)
#define IS_HASH(_x_) ((_x_) == CHAR_HS)
#define IS_WHITE_SPACE(_x_) (((_x_) == CHAR_SP) || ((_x_)== CHAR_TAB))
#define IS_WHITE_PREFIX(_x_) (!(_x_) || IS_HASH(_x_) || IS_NEWLINE(_x_))

/* print an error message */
#if defined(DEBUG)
#define ERROR(_fmt_, ...) do { \
	char err_prefix[256]; \
	snprintf(err_prefix, sizeof(err_prefix), "%s:%d, %s(): ", __FILE__, \
		__LINE__, __FUNCTION__); \
	print_error(err_prefix, "", _fmt_, ##__VA_ARGS__); \
} while (0)
#else
#define ERROR(_fmt_, ...) do { \
	print_error("", "", _fmt_, ##__VA_ARGS__); \
} while (0)
#endif

/* verify an external application used by backup exists. If it doesn't, abort */
#define TEST_EXTERNAL_APP_EXISTS(_app_, ...) do { \
	if (sys_exec("which " _app_ " > /dev/null", ##__VA_ARGS__)) { \
		print_error("Can't find: ", "Aborting...", _app_, \
			##__VA_ARGS__); \
		exit(1); \
	} \
} while (0)

#define IS_YES_NO_ANSWER(_ans_) \
	(((MAX(strlen(_ans_), strlen(STR_YES)) == strlen(STR_YES)) && \
	  (!strncasecmp(_ans_, STR_YES, MIN(strlen(_ans_), \
	  strlen(STR_YES))))) || ((MAX(strlen(_ans_), \
	  strlen(STR_NO)) == strlen(STR_NO)) && \
	  !strncasecmp(_ans_, STR_NO, MIN(strlen(_ans_), strlen(STR_NO)))))

typedef enum {
	BACKUP_ERR_BACKUP = 1<<0,
	BACKUP_ERR_EDIT = 1<<1,
	BACKUP_ERR_VERSION = 1<<2,
	BACKUP_ERR_HELP = 1<<3,
	BACKUP_ERR_ARGS = 1<<4,
	BACKUP_ERR_NOTEXIST = 1<<5,
} backup_err_t;

typedef struct path_t {
	struct path_t *next;
	char *str;
} path_t;

typedef int (*file_processing_handler_t)(int is_exit, char *path,
	void **data);

static char *home_dir;
static char backup_tar_gz[MAX_PATH_LEN];
static char working_dir[MAX_PATH_LEN];
static char backup_dir[MAX_PATH_LEN];
static char backup_conf[MAX_PATH_LEN];
static int supress_conf_cleanup;

/* common format for printing an error */
static int print_error(char *err_prefix, char *err_suffix, char *fmt, ...)
{
#define MAX_ERR_LN 256
	va_list ap;
	char err_str[MAX_ERR_LN];

	memset(err_str, 0, MAX_ERR_LN);
	va_start(ap, fmt);
	vsnprintf(err_str, MAX_ERR_LN, fmt, ap);
	va_end(ap);

	return fprintf(stderr, "%s" TEXT_ERROR("%s") "%s%s\n", err_prefix,
		err_str, *err_suffix ? ". " : "", err_suffix);
}

/* open a file and set its user id and group id */
static FILE *open_id(char *file_name, uid_t uid, gid_t gid)
{
	FILE *file;
	int ret;

	if (!(file = fopen(file_name, "w+")))
		return NULL;

	ret = chown(file_name, uid, gid);
	if (ret == -1) {
		ERROR("could not chown() %s", file_name);
		fclose(file);
		exit(1);
	}
	return file;
}

static path_t *path_alloc(char *str)
{
	path_t *path;
	int len;

	if (!(path = (path_t*)calloc(1, sizeof(path_t))))
		goto Error;

	len = strlen(str) + 1;
	if (!(path->str = (char*)calloc(len, sizeof(char))))
		goto Error;

	snprintf(path->str, len, "%s", str);
	return path;

Error:
	if (path)
		free(path);

	return NULL;
}

static void path_free(path_t *path)
{
	if (!path)
		return;

	if (path->str)
		free(path->str);
	free(path);
}

/*
 * removes a newline if it comes as the last non zero charcater in str
 * return: 0 - if the string was not modified
 * 1 - if the string was modified
 */
static int remove_newline(char *str)
{
	char *eol = str + strlen(str) - 1;

	if (*eol != '\n')
		return 0;

	*eol = 0;
	return 1;
}

/* adds a newline at the end of str if a the resulting string will still be
 * null terminated.
 * return:
 * 0 - if the string was not modified
 * 1 - if the string was modified
 */
static int add_newline(char *str)
{
	char *eol = str + strlen(str);

	if (*(eol + 1))
		return 0;

	*eol = '\n';
	return 1;
}

/* test if a line in the conf file is skippable */
static int is_whiteline(char *line, int len)
{
	int i, wl;

	for (i = 0; i < len && !(wl = IS_WHITE_PREFIX(line[i])) && 
		IS_WHITE_SPACE(line[i]); i++);

	return i == len ? 1 : wl;
}

/* find the first non white space character in a path buffer */
static char *del_leading_white(char *path, int len)
{
	char *start = path;

	while (path && path - start < len && IS_WHITE_SPACE(*path))
		path++;
	return path;
}

static int is_del_obsolete_entry(char *path)
{
#define INPUT_SZ 5

	char ans[INPUT_SZ];
	int ret, is_first = 1;

	printf(TEXT_HIGHLIGHT("%s") " does not exist...\n", path);
	printf("do you want to remove it from the configuration file? [Y/n] ");

	/* repeat while answer provided by the user is not a case insensative 
	   substring of 'yes' or 'no' */
	do {
		int is_newline_found;
		char *ret;

		if (!is_first)
			printf("Please answer Y[es] or N[o]: ");
		else
			is_first = 0;

		/* get user's answer */
		bzero(ans, INPUT_SZ);
		ret = fgets(ans, INPUT_SZ, stdin);
		if (!ret) {
			ERROR("could not read stdin");
			exit(1);
		}
		is_newline_found = remove_newline(ans);

		/* if a newline was not encountered,
		   it must be found and removed from stdin */
		if (!is_newline_found)
			while (!IS_NEWLINE(fgetc(stdin)));
	} while (!IS_YES_NO_ANSWER(ans));

	switch (*ans) {
	case 0: /* a newline was replaced by the null terminator */
	case 'y':
	case 'Y':
		ret = 1;
		break;
	case 'n':
	case 'N':
		ret = 0;
		break;
	default:
		ERROR("unreachable switch case");
		exit(1);
		break;
	}
	return ret;
}

/* copy content from one file to another */
static int cp_file(FILE *to, FILE *from)
{
	char line[MAX_PATH_LEN];

	bzero(line, MAX_PATH_LEN);
	while (fgets(line, MAX_PATH_LEN, from)) {
		fputs(line, to);
		bzero(line, MAX_PATH_LEN);
	}

	return fseek(from, 0, SEEK_SET) || fseek(to, 0, SEEK_SET);
}

/* configuration file cleanup callback function */
static int handler_conf_cleanup(int is_exit, char *path, void **data)
{
	FILE *cnf = *((FILE**)data);
	char *path_ptr = NULL;
	int wl = is_whiteline(path, MAX_PATH_LEN);

	path_ptr = del_leading_white(path, MAX_PATH_LEN);

	if (!wl && !is_exit && is_del_obsolete_entry(path_ptr))
		return 0; /* ignore this path, don't insert it in the new conf
			     file */
	add_newline(path);
	fputs(path, cnf);
	return 0;
}

/* path data structure creation callback function */
static int handler_paths_create(int is_exit, char *path, void **data)
{
	path_t *new;

	if (!is_exit)
		return 0;

	if (!(new = path_alloc(del_leading_white(path, MAX_PATH_LEN))))
		return -1;

	new->next = *((path_t**)data);
	*data = new;

	return 0;
}

/* generic function for processing configuration file lines (paths) */
static int file_process_genric(FILE *file, file_processing_handler_t handler, 
	void **data)
{
	char path[MAX_PATH_LEN];

	bzero(path, MAX_PATH_LEN);
	while (fgets(path, MAX_PATH_LEN, file)) {
		char *path_ptr;
		int is_exit;
		struct stat st;

		/* line is not whitespaces only or a comment */
		path_ptr = del_leading_white(path, MAX_PATH_LEN);
		remove_newline(path_ptr);
		if (!(is_exit = !stat(path_ptr, &st)) && errno != ENOENT) {
			ERROR("stat(%s, &st), continuing...", path_ptr);
			bzero(path, MAX_PATH_LEN);
			continue;
		}

		if (handler(is_exit, path, data))
			return -1;
		bzero(path, MAX_PATH_LEN);
	}
	return 0;
}

/* allow user to remove stale paths from the configuration file */
static int conf_cleanup(void)
{
	char backup_conf_bck[MAX_PATH_LEN];
	FILE *cnf, *tmp, *bck;
	struct stat st;

	if (supress_conf_cleanup)
		return 0;

	/* open cnf FILE */
	if (!(cnf = fopen(backup_conf, "r+"))) {
		ERROR("fopen(%s, \"r+\")", backup_conf);
		return -1;
	}
	/* open tmp FILE */
	if (!(tmp = tmpfile())) {
		ERROR("tmpfile()");
		fclose(cnf);
		return -1;
	}

	if (stat(backup_conf, &st) == -1) {
		ERROR("stat(%s)", backup_conf);
		fclose(cnf);
		fclose(tmp);
		return -1;
	}

	/* open bck FILE */
	snprintf(backup_conf_bck, MAX_PATH_LEN, "%s.bck", backup_conf);
	if (!(bck = open_id(backup_conf_bck, st.st_uid, st.st_gid))) {
		ERROR("open_id(%s, %d, %d)", backup_conf_bck, st.st_uid,
			st.st_gid);
		fclose(cnf);
		fclose(tmp);
		return -1;
	}

	/* copy conf file to backup file in case of program termination while
	 * conf file is being manipulated */
	cp_file(bck, cnf);
	fclose(bck);

	/* copy conf file to tmp file and create a new conf file */
	cp_file(tmp, cnf);
	fclose(cnf);
	if (remove(backup_conf)) {
		ERROR("remove(%s)", backup_conf);
		fclose(tmp);
		return -1;
	}
	if (!(cnf = open_id(backup_conf, st.st_uid, st.st_gid))) {
		ERROR("open_id(%s, %d, %d)", backup_conf, st.st_uid, st.st_gid);
		fclose(tmp);
		return -1;
	}
	file_process_genric(tmp, handler_conf_cleanup, (void**)&cnf);
	fclose(tmp);
	fclose(cnf);
	if (remove(backup_conf_bck))
		ERROR("remove(%s)", backup_conf_bck);

	return 0;
}

/* build a paths linked list */
static int paths_create(path_t **cp_paths)
{
	int ret;
	FILE *cnf;

	/* open cnf FILE */
	if (!(cnf = fopen(backup_conf, "r+"))) {
		ERROR("fopen(%s, \"r+\")", backup_conf);
		return -1;
	}
	ret = file_process_genric(cnf, handler_paths_create, (void**)cp_paths);
	fclose(cnf);

	return ret;
}

/* system execution function */
static int sys_exec(char *fmt, ...)
{
	int status = 0;

	if (!fork()) { /* child process */
		char command[MAX_PATH_LEN];
		va_list ap;
		int ret;
		char *args[4];

		bzero(command, MAX_PATH_LEN);
		va_start(ap, fmt);
		ret = vsnprintf(command, MAX_PATH_LEN - 1, fmt, ap);
		va_end(ap);

		if (ret < 0)
			exit(1);

		args[0] = getenv(ENV_SHELL);
		args[1] = "-c";
		args[2] = command;
		args[3] = NULL;
		execvp(args[0], args);
	}

	wait(&status);

	/* fail if the child process did not exit gracefully */
	return WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -1;
}

/* initialize global variables */
static int init(void)
{
	if (!(home_dir = getenv(ENV_HOME))) {
		ERROR("getenv()");
		return -1;
	}
	if (!getcwd(working_dir, MAX_PATH_LEN)) {
		ERROR("getcwd()");
		return -1;
	}
	if ((snprintf(backup_tar_gz, MAX_PATH_LEN, "%s/backup.tar.gz",
		working_dir)) < 0) {
		ERROR("snprintf()");
		return -1;
	}

	if (snprintf(backup_conf, MAX_PATH_LEN, "%s/.backup.conf",
		home_dir) < 0) {
		return -1;
	}

	return 0;
}

/* create temporary backup dir */
static int create_backup_dir(void)
{
#define BACKUP_DIR "backup"

	int ret, i = 0;
	char *tmp;

	bzero(backup_dir, MAX_PATH_LEN);
	tmp = backup_dir;
	/* add leading underscores if the backup directory allready exists */
	do {
		snprintf(&(tmp[i]), MAX_PATH_LEN - (i + i), BACKUP_DIR);
		if ((ret = mkdir(backup_dir, 0777)) == -1) {
			if (errno != EEXIST) {
				ERROR("mkdir()");
				return -1;
			}

			tmp[i] = '_';
		}
		i++;
	} while (ret && (i < (MAX_PATH_LEN - strlen(backup_conf) - 1)));

	return ret;
}

/* copy configuration file locations to backup directory */
static int copy_to_backup_dir(void)
{
	path_t *cp_paths = NULL;
	int ret = 0;

	conf_cleanup(); /* XXX handle errors */
	paths_create(&cp_paths); /* XXX handle errors */

	/* copy backup destinations to backup_dir */
	printf("backing up...\n");
	while (cp_paths) {
		path_t *tmp;

		if (sys_exec("cp -pr --parents %s %s",
			cp_paths->str, backup_dir)) {
			ERROR("cp -pr --parents %s %s", cp_paths->str,
				backup_dir);
			ret = -1;
		}
		tmp = cp_paths;
		cp_paths = cp_paths->next;
		path_free(tmp);
	}

	return ret;
}

/* make a *.gar.gz of the temporary backup directory */
static int make_tar_gz(void)
{
	if (sys_exec("cd %s && tar czfp %s .", backup_dir, backup_tar_gz))
		return -1;

	printf("done: " TEXT_HIGHLIGHT("%s") "\n", backup_tar_gz);
	return 0;
}

/* remove the backup directory */
static int remove_backup_dir(void)
{
	return sys_exec("rm -rf %s", backup_dir) ? -1 : 0;
}

/* test if a configuration file stated explicitly by the user exists */
static int is_optional_backup_conf_exit(char *file_name)
{
	struct stat st;

	snprintf(backup_conf, MAX_PATH_LEN, "%s", file_name);
	return stat(backup_conf, &st) ? 0 : 1;
}

/* parse user arguments */
static int get_args(int argc, char *argv[])
{
#define OPTSTRING "b::evhf"

	unsigned int ret = 0;
	char opt;

	while ((opt = (char)getopt(argc, argv, OPTSTRING)) != -1) {
		switch (opt) {
		case 'b': /* do backup */

			/* conflicting arguments:
			   - multiple do backup
			   - do edit
			   - get help
			   - get version */
			if (ret & (BACKUP_ERR_BACKUP | BACKUP_ERR_EDIT | 
				BACKUP_ERR_HELP | BACKUP_ERR_VERSION)) {
				return BACKUP_ERR_ARGS;
			}
			if (optarg && !is_optional_backup_conf_exit(optarg))
				return BACKUP_ERR_NOTEXIST;
			ret |= BACKUP_ERR_BACKUP;
			break;

		case 'e': /* do edit */
			/* conflicting arguments:
			   - do backup
			   - multiple do edit
			   - get help
			   - get version */
			if (ret & (BACKUP_ERR_BACKUP | BACKUP_ERR_EDIT | 
				BACKUP_ERR_HELP | BACKUP_ERR_VERSION)) {
				return BACKUP_ERR_ARGS;
			}
			ret |= BACKUP_ERR_EDIT;
			break;

		case 'v': /* get version */
			/* conflicting arguments:
			   - do backup
			   - do edit
			   - get help
			   - multiple get version */
			if (ret & (BACKUP_ERR_BACKUP | BACKUP_ERR_EDIT | 
				BACKUP_ERR_HELP | BACKUP_ERR_VERSION)) {
				return BACKUP_ERR_ARGS;
			}
			ret |= BACKUP_ERR_VERSION;
			break;

		case 'h': /* get help */
			/* conflicting arguments:
			   - do backup
			   - do edit
			   - multiple get help
			   - get version */
			if (ret & (BACKUP_ERR_BACKUP | BACKUP_ERR_EDIT | 
				BACKUP_ERR_HELP | BACKUP_ERR_VERSION)) {
				return BACKUP_ERR_ARGS;
			}
			ret |= BACKUP_ERR_HELP;
			break;

		case 'f': /* force no configuration file cleanup */
			/* conflicting arguments:
			   - multiple force configuration cleanup
			   - get help
			   - get version */
			if (supress_conf_cleanup || ret & (BACKUP_ERR_HELP | 
				BACKUP_ERR_VERSION)) {
				return BACKUP_ERR_ARGS;
			}
			supress_conf_cleanup = 1;
			break;
		default:
			return BACKUP_ERR_ARGS;
			break;
		}
	}
	return ret;
}

/* trim possible parameters off a command line, return the command name only */
static char *cmdline_trim(char *str, char *buffer, int len)
{
	snprintf(buffer, len, "%s", str);
	strtok(buffer, " ");
	return buffer;
}

/* trim a full path down to the file / bottom directory name */
static char *app_name_get(char *path)
{
	char *app_name;

	for (app_name = path + strlen(path) - 1; 
		app_name >= path && *app_name != '/'; app_name--);

	return ++app_name;
}

/* backup the locations stated in the configuration file */
static int backup(void)
{
	TEST_EXTERNAL_APP_EXISTS("tar");

	return create_backup_dir() || copy_to_backup_dir() || make_tar_gz() || 
		remove_backup_dir() ? -1 : 0;
}

/* edit the locations stated in the configuration file */
static int edit(void)
{
	char *editor = DEFAULT_EDITOR;
	char *diff_prog = DEFAULT_DIFFPROG;
	char *env_var;
	char backup_conf_old[MAX_PATH_LEN];
	char external_app_name[MAX_PATH_LEN];
	FILE *old, *cnf;
	struct stat st;
	int is_new;

	bzero(backup_conf_old, MAX_PATH_LEN);
	snprintf(backup_conf_old, MAX_PATH_LEN, "%s.old", backup_conf);

	is_new = stat(backup_conf, &st) == -1 ? 1 : 0;
	if (!(cnf = fopen(backup_conf, "a+"))) {
		ERROR("fopen(%s, \"a+\")", backup_conf);
		return -1;
	}
	if (!(old = fopen(backup_conf_old, "w+"))) {
		ERROR("fopen(%s, \"w+\")", backup_conf_old);
		fclose(cnf);
		return -1;
	}

	cp_file(old, cnf);
	fclose(old);
	fclose(cnf);

	if ((env_var = getenv(ENV_EDITOR)))
		editor = env_var;
	if ((env_var = getenv(ENV_DIFFPROG)))
		diff_prog = env_var;

	TEST_EXTERNAL_APP_EXISTS("%s", cmdline_trim(editor, external_app_name, 
		MAX_PATH_LEN));
	TEST_EXTERNAL_APP_EXISTS("%s", cmdline_trim(diff_prog,
		external_app_name, MAX_PATH_LEN));

	sys_exec("%s %s", editor, backup_conf);
	if (!is_new) {
		sys_exec("if [ -n \"`diff %s %s`\" ]; then %s %s %s; else " \
			"echo \"%s has not been modified\"; fi",
			backup_conf_old, backup_conf, diff_prog,
			backup_conf_old, backup_conf, backup_conf);
	}
	if (remove(backup_conf_old))
		ERROR("remove(%s)", backup_conf_old);
	conf_cleanup();

	return 0;
}

/* print application version */
static void version(void)
{
#define VER_LENGTH 32
	char ver[VER_LENGTH];

#ifdef VERSION
	snprintf(ver, VER_LENGTH, TEXT_HIGHLIGHT("%s"), VERSION);
#else
	snprintf(ver, VER_LENGTH, "no data is available");
#endif

	printf("backup version: %s\n", ver);
}

/* print usage message */
static void usage(char *app_path)
{
	char *app_name = app_name_get(app_path);

	printf(
		"usage:\n"
		"          " TEXT_HIGHLIGHT(
			"%s < -e | -b [ conf_file ] > [ -f ]") "\n"
		"          " TEXT_HIGHLIGHT("%s < -v | -h >") "\n"
		"\n"
		"   where\n"
		"   " TEXT_HIGHLIGHT("-e") "  edit the configuration file.\n"
		"       Enter the full paths to directories or files you "
		"wish to backup.\n"
		"       A line can be commented out by using the " 
		TEXT_HIGHLIGHT("#") " character.\n"
		"       Set the environment variable " 
			TEXT_UNDERLINE("EDITOR") " to "
		"use an editor of your choice.\n"
		"       If " TEXT_UNDERLINE("EDITOR") " is not set, " 
		TEXT_HIGHLIGHT("%s") " will use " TEXT_HIGHLIGHT("%s")
			" for editing the configuration\n"
		"       file.\n"
		"       Once an existing configuration file is modified, "
		TEXT_HIGHLIGHT("%s") " displays a diff\n"
		"       between the previous and the current versions.\n"
		"       Set the environment variable "
			TEXT_UNDERLINE("DIFFPROG") " to "
		"use a diff program of your\n"
		"       choice. If " TEXT_UNDERLINE("DIFFPROG") " is not set, "
		TEXT_HIGHLIGHT("%s") " will use " TEXT_HIGHLIGHT("%s") ".\n"
		"   " TEXT_HIGHLIGHT("-b") "  backup the files and directories "
		"mentioned in the configuration file.\n"
		"       " TEXT_HIGHLIGHT("%s")
			" uses the default configuration file "
		"(%s)\n"
		"       unless a " TEXT_HIGHLIGHT("conf_file") " is stated "
		"specifically.\n"
		"       The gzipped tarball " TEXT_HIGHLIGHT("%s.tar.gz")
			" containing the backed up files will be\n"
		"       placed in the working directory.\n"
		"   " TEXT_HIGHLIGHT("-f")
			"  Do not prompt to clean configuration file of "
			"redundant paths.\n"
		"   " TEXT_HIGHLIGHT("-v") "  Display " 
		TEXT_HIGHLIGHT("%s") " version.\n"
		"   " TEXT_HIGHLIGHT("-h") "  Print this message and exit.\n"
		"\n"
		"IAS, October 2004\n", app_name, app_name, app_name,
		DEFAULT_EDITOR, app_name, app_name, DEFAULT_DIFFPROG, app_name,
		backup_conf, app_name, app_name); 
}

int main(int argc, char *argv[])
{
	int ret = 0;

	init();

	switch (get_args(argc, argv)) {
	case BACKUP_ERR_BACKUP:
		ret = backup();
		break;
	case BACKUP_ERR_EDIT:
		ret = edit();
		break;
	case BACKUP_ERR_VERSION:
		version();
		break;
	case BACKUP_ERR_HELP:
		usage(argv[0]);
		break;
	case BACKUP_ERR_NOTEXIST:
		fprintf(stderr, "file '%s' does not exit\n", backup_conf);
		break;
	case BACKUP_ERR_ARGS: /* fall through */
	default:
		fprintf(stderr, "try '%s -h' for more information\n",
			app_name_get(argv[0]));
		break;
	}

	return ret;
}

