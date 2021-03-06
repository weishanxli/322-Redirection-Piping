/* 
 * tsh - A tiny shell program with job control
 * 
 * Weishan Li, 30755725
 * Jack DeGuglielmo, 30900481
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             /* print help message */
            usage();
	    break;
        case 'v':             /* emit additional diagnostic info */
            verbose = 1;
	    break;
        case 'p':             /* don't print a prompt */
            emit_prompt = 0;  /* handy for automatic testing */
	    break;
	default:
            usage();
	}
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */
    
 /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1) {

	/* Read command line */
	if (emit_prompt) {
	    printf("%s", prompt);
	    fflush(stdout);
	}
	if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
	    app_error("fgets error");
	if (feof(stdin)) { /* End of file (ctrl-d) */
	    fflush(stdout);
	    exit(0);
	}

	/* Evaluate the command line */
	eval(cmdline);
	fflush(stdout);
	fflush(stdout);
    } 

    exit(0); /* control never reaches here */
}
  
/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
*/
void eval(char *cmdline) 
{
	char *argv[MAXARGS];
	char buf[MAXLINE];
	int bg;
	pid_t pid;
	
	//create signal mask to block SIGCHLD signals later
	sigset_t mask, pmask;
	sigaddset(&mask, SIGCHLD);

	//coppying cmdline to buf, bg set to 1/0 depending if '&' found in buf
	strcpy(buf, cmdline);
	bg = parseline(buf, argv);
	

	if (argv[0] == NULL){
		return;
	}
	
	//checking for builtin commands
	if (!builtin_cmd(argv)){
		//blocking SIGINT signals
		sigprocmask(SIG_BLOCK, &mask, &pmask);
		
		//fork a child process
		if ((pid = fork()) == 0){
			
			//--------------checking for redirects---------------
			int i = 0;
			while (argv[i] != NULL) {

				//redirect next filename to be stdin
				if (!strcmp(argv[i], "<")) {

					//open file and set flags/permissions, returns file descriptor of file
					int fd0 = open(argv[i+1], O_RDONLY);
					//set file to stdin
	    			dup2(fd0, 0);
	    			//close file
	    			close(fd0);
	    			argv[i] = NULL;
	    			argv[i+1] = NULL;
	    			i=i+2;
				} 
				//redirect next filename to be stdout 
				else if (!strcmp(argv[i], ">")) {

					//open file and set flags/permissions, returns file descriptor of file
					int fd1 = open(argv[i+1], O_WRONLY|O_TRUNC|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO);
	    			//set file to stdout
	    			dup2(fd1, 1);
	    			close(fd1);
	    			argv[i] = NULL;
	    			argv[i+1] = NULL;
	    			i=i+2;
				}
				//redirect next filename to be stdout, but append instead of overwrite
				else if (!strcmp(argv[i], ">>")) {
					int fd2 = open(argv[i+1], O_WRONLY|O_APPEND|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO);
					dup2(fd2, 1);
					close(fd2);
					argv[i] = NULL;
	    			argv[i+1] = NULL;
	    			i=i+2;
				}
				//redirect stderr to next filename
				else if (!strcmp(argv[i], "2>")) {
					int fd3 = open(argv[i+1], O_WRONLY|O_CREAT,S_IRWXU|S_IRWXG|S_IRWXO|O_TRUNC);
					//set file to stderr
					dup2(fd3, 2);
					close(fd3);
					argv[i] = NULL;
	    			argv[i+1] = NULL;
	    			i=i+2;
				}
				else {
					i++;
				}	
			}
			//-------------end redirects------------

			//unblock signals
			sigprocmask(SIG_SETMASK, &pmask, NULL);
			//setting the process's group id
			setpgid(0,0);

			//-------------piping-----------------
			int numPipes = 0;
			while (argv[i] != NULL) {			//Count number of pipes in command
				if (!strcmp(argv[i], "|")) {
					numPipes++;
				}	
			}

			int p = 0;			//initialize iterator, pipe boolean, previous pipe index and previous pipe read fd
			int finalPipeIndex = 0;
			int isPipe = 0;
			int prevPipe = 0;
			while (argv[p] != NULL) {				//loop through command
				
				if (!strcmp(argv[p], "|")) {			//stop on pipe
					isPipe = 1;	
					int pd[2];			//initialize pipe
			        	pipe(pd);
					dup2(pd[0],0);			//set input of parent to read of pipe
					argv[p] = NULL;
					
					
		        		if (!fork()) {			//fork for command before pipe
		            			dup2(pd[1], 1); 		// remap output back to parent
		            			if(finalPipeIndex == 0) {	// edge case for first pipe
							execve(argv[0], argv, environ);
						} else {
							dup2(prevPipe, 0);		//case for middle command (map previous pipe to stdin)
							execve(argv[finalPipeIndex+1], &argv[finalPipeIndex + 1], environ);	//exec cammand after last pipe
							close(prevPipe);		//close prev pipe
						}
					
						close(pd[1]);
						exit(0);
			        }
				
				prevPipe = pd[0];			// store perious pipe read for next iteration 
			        
				//close unused pipes
			        close(pd[1]);

				//set pipe index to next pipe
				finalPipeIndex = p;
				}
				p++;
			}


			//-------------end piping-----------------

			//executing command
			if(isPipe == 1){
				//execute last command of pipes
				execve(argv[finalPipeIndex + 1], &argv[finalPipeIndex+1], environ);
				exit(0);
			}else if (execve(argv[0], argv, environ) < 0) {
				printf("%s: Command not found.\n", argv[0]);
				exit(0);
				
			}
			
		}

		//if process in foreground
		if (!bg) {
			//add the job to the joblist
			addjob(jobs, pid, FG, cmdline);
			//unblock the signals
			sigprocmask(SIG_SETMASK, &pmask, NULL);
			//parent waits til child process finishes
			waitfg(pid);
		}
		//if process in background
		else {
			//add job to job the joblist
			addjob(jobs, pid, BG, cmdline);
			//unblock the signals
			sigprocmask(SIG_SETMASK, &pmask, NULL);
			printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline);
		
		}	
	}
    return;
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv) 
{
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to first space delimiter */
    int argc;                   /* number of args */
    int bg;                     /* background job? */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
	buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
	buf++;
	delim = strchr(buf, '\'');
    }
    else {
	delim = strchr(buf, ' ');
    }

    while (delim) {
	argv[argc++] = buf;
	*delim = '\0';
	buf = delim + 1;
	while (*buf && (*buf == ' ')) /* ignore spaces */
	       buf++;

	if (*buf == '\'') {
	    buf++;
	    delim = strchr(buf, '\'');
	}
	else {
	    delim = strchr(buf, ' ');
	}
    }
    argv[argc] = NULL;
    
    if (argc == 0)  /* ignore blank line */
	return 1;

    /* should the job run in the background? */
    if ((bg = (*argv[argc-1] == '&')) != 0) {
	argv[--argc] = NULL;
    }
    return bg;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv) 
{
	//initializing strings to compare first string of cmdline
	char bgStr[] = "bg";
	char fgStr[] = "fg";
	char quitStr[] = "quit";
	char jobsStr[] = "jobs";
	
	if(!strcmp(argv[0], fgStr) | !strcmp(argv[0], bgStr)){	//fg or bg state (calls do_bgfg)
		do_bgfg(argv);
		return 1;
	}else if(!strcmp(argv[0], quitStr)){		//quit state (exits)
		exit(0);
	}else if(!strcmp(argv[0], jobsStr)){		//job state (calls given listjobs())
		listjobs(jobs);
		return 1;
	}
    return 0;     /* not a builtin command */
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) 
{
	int jid;
	pid_t pid;
	char *pidOrjid;

	pidOrjid = argv[1];

	//if command has nonexistent id after fg/bg
	if (pidOrjid == NULL) {
		printf("%s command requires PID or %%jobid argument\n", argv[0]);
		return;
	}

	//checks if jid
	if (pidOrjid[0] == '%') {

		//second argument after '%' set to integer value of jid from argv
		jid = atoi(&pidOrjid[1]);

		//check if job is nonexistant
		if(getjobjid(jobs, jid) == NULL){
			printf("%s: No such job\n", pidOrjid);
			return;
		} else {
			pid = getjobjid(jobs, jid)->pid;

			//send continue signal
			kill(-pid, SIGCONT);

			//if fg input, set bg process state to fg
			if (!strcmp("fg", argv[0])) {
				getjobpid(jobs, pid)->state = FG;
				waitfg(pid);
			}

			//if bg input, set fg process state to bg
			if (!strcmp("bg", argv[0])) {
				struct job_t *job;
				job = getjobpid(jobs, pid);
				printf("[%d] (%d) %s", job->jid, job->pid, job->cmdline);
				job->state = BG;
			}
		}
	} 
	//checks if pid
	else if(isdigit(pidOrjid[0])) {

		//get integer value of pid from argv
		pid = atoi(pidOrjid);

		//check if job is nonexistant
		if(getjobpid(jobs, pid) == NULL){
			printf("(%d): No such process\n", pid);
			return;
		}

		//send continue signal
		kill(-pid, SIGCONT);

		//if fg input, set bg process state to fg
		if (!strcmp("fg", argv[0])) {
			getjobpid(jobs, pid)->state = FG;
			waitfg(pid);
		}

		//if bg input, set fg process state to bg
		if (!strcmp("bg", argv[0])) {
			struct job_t *job;
			job = getjobpid(jobs, pid);
			printf("[%d] (%d) %s", job->jid, job->pid, job->cmdline);
			job->state = BG;
		}

	} else {
		//argv[0] equals FG/BG
		printf("%s: argument must be a PID or %%jobid\n", argv[0]);
		return;
	}
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
	int doesJobExist = 0;
	int i;
	for (i=0; i<MAXJOBS; i++){		//loops through job list
		if (jobs[i].pid == pid){
			doesJobExist = 1;		//if pid is in joblist, condition set to true
			break;
		}
	}
	if(doesJobExist){
		while(pid == fgpid(jobs)){
			sleep(1);					//busy while loop until fg job is no longer in list
		}
	}
    return;
}

/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig) 
{
	pid_t pid;
	int status;
	//WNOHANG returns immediately if no child exits, WUNTRACED returns if child has stopped
	while((pid = waitpid(-1, &status, WNOHANG|WUNTRACED)) > 0){
		if (WIFEXITED(status) != 0){		//true if child has terminated normally
			deletejob(jobs, pid);		//deletes terminated job
		}
		if (WIFSTOPPED(status) != 0){ //true if child process was stopped by delivery of signal
			getjobpid(jobs, pid)->state = ST; //change state to stopped
			printf("Job [%d] (%d) stopped by signal %d\n", pid2jid(pid), pid, WSTOPSIG(status));
		}
		if (WIFSIGNALED(status)){ //true if child process was terminated by delivery of signal
			printf("Job [%d] (%d) terminated by signal %d\n", pid2jid(pid), pid, WTERMSIG(status));
			deletejob(jobs, pid);	//deletes terminated job
		}
	}
    return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) 
{
	int i=0;
	//finding foreground job
	for(i=0;i<MAXJOBS;i++){		//loops through jobs until pid = 0 (last job)
		if(jobs[i].pid != 0){
			if(jobs[i].state == FG){		//finds job in FG
				kill(-jobs[i].pid, sig);		//send kill to gpid for fg jobs
				return;
			}
		}
	}
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{

	int i=0;
	//finding foreground job
	for(i=0;i<MAXJOBS;i++){
		if(jobs[i].state == FG){
			if(jobs[i].pid != 0){
				//sending SIGTSTP signal to foreground job
				kill(-jobs[i].pid, sig);
				return;
			}
		}
	}
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs) 
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid > max)
	    max = jobs[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
    int i;
    
    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid == 0) {
		    jobs[i].pid = pid;
		    jobs[i].state = state;
		    jobs[i].jid = nextjid++;
		    if (nextjid > MAXJOBS)
			nextjid = 1;
		    strcpy(jobs[i].cmdline, cmdline);
	  	    if(verbose){
		        printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
	        }
	        return 1;
		}
    }
    printf("Tried to create too many jobs\n");
    return 0; 
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == pid) {
	    clearjob(&jobs[i]);
	    nextjid = maxjid(jobs)+1;
	    return 1;
	}
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].state == FG)
	    return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid)
	    return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid == jid)
	    return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid) {
            return jobs[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) 
{
    int i;
    
    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid != 0) {
	    printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
	    switch (jobs[i].state) {
		case BG: 
		    printf("Running ");
		    break;
		case FG: 
		    printf("Foreground ");
		    break;
		case ST: 
		    printf("Stopped ");
		    break;
	    default:
		    printf("listjobs: Internal error: job[%d].state=%d ", 
			   i, jobs[i].state);
	    }
	    printf("%s", jobs[i].cmdline);
	}
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
	//hello this is a test of stash

}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
	unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) 
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}



