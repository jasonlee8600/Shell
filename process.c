#include "process.h"


// FUNCTION DECLARATIONS
// handles SIMPLE commands
int simple_command(const CMD *cmdList);
// handle fromType (redirecting stdin)
void redirect_stdin(const CMD *cmdList);
// handle toType (redirecting stdout)
void redirect_stdout(const CMD *cmdList);
// handles PIPE commands
int pipe_command(const CMD *cmdList);
// handles SEP_AND commands
int and_command(const CMD *cmdList);
// handles SEP_OR commands
int or_command(const CMD *cmdList);
// handles SUBCMD (subcommands)
int sub_command(const CMD *cmdList);
// handles SEP_END commands
int end_command(const CMD *cmdList);
// handles SEP_BG commands
int background_command(const CMD *cmdList);
// helper for SEP_BG function
int background_command_helper(const CMD *cmdList);
// set env variable $? to whatever status is passed in
void env_variable(int status);
// handles built-in commands
int built_in_command(const CMD *cmdList);
// handle fromType and toType for built-ins, only difference from other one is it returns instead of exit() because not in a child of a fork
int redirect_stdin_builtin(const CMD *cmdList);
int redirect_stdout_builtin(const CMD *cmdList);
// handles pushd commands
int pushd_command(const CMD *cmdList);
// handles popd commands
int popd_command(const CMD *cmdList);


// global linked list for pushd and popd
typedef struct node {
    char* directory;
    struct node* next;
} Node;

// initialize linked list
    // make helper to malloc and set fields for nodes?
Node *head = NULL;


int process (const CMD *cmdList) {


    // reap zombies
    int pid = 1;
    int status;
    while (pid > 0) {
        pid = waitpid(-1, &status, WNOHANG);
        if (pid > 0) {
            int f = fprintf(stderr, "Completed: %d (%d)\n", pid, status);
            // if fprintf() error
            if (f < 0) {
                int errno2 = errno;
                perror("fprintf() error");
                env_variable(f);
                return errno2;
            }
        }
    }

    // check if passed null cmdList
    if (cmdList == NULL) {
        return 0;
    }

    // var to hold return value of switch cases
    int ret_val;
    switch(cmdList->type) {
        case SIMPLE:
            if (strcmp(cmdList->argv[0], "cd") == 0 || strcmp(cmdList->argv[0], "pushd") == 0 || strcmp(cmdList->argv[0], "popd") == 0) {
                ret_val = built_in_command(cmdList);
                break;
            }
            ret_val = simple_command(cmdList);
            break;
        
        // '|'
        case PIPE:
            ret_val = pipe_command(cmdList);
            break;

        // '&&'
        case SEP_AND:
            ret_val = and_command(cmdList);
            break;

        // '||'
        case SEP_OR:
            ret_val = or_command(cmdList);
            break;

        case SUBCMD:
            ret_val = sub_command(cmdList);
            break;

        // ';'
        case SEP_END:
            ret_val = end_command(cmdList);
            break;

        // '&'
        case SEP_BG:
            ret_val = background_command(cmdList);
            break;

        default:
            break;
    }


    env_variable(ret_val);
    return ret_val;
}


int simple_command(const CMD *cmdList) {

    int pid = fork();

    // fork failure returns -1
    if (pid < 0) {
        int errno2 = errno;
        // message to stderr
        perror("Fork failure");
        // unsuccessful program execution
        return errno2;
    }

    // child
    if (pid == 0) {

        // set local vars
        for (int i = 0; i < cmdList->nLocal; i++) {
            setenv(cmdList->locVar[i], cmdList->locVal[i], 1);
        }
        
        // handle fromType (redirecting stdin)
        redirect_stdin(cmdList);
        // handle toType
        redirect_stdout(cmdList);

        // have the child call execvp to replace the currently executing code and data 
        // with an instance of the code and data of the new process, 
        // passing some command line arguments.
        int x = execvp(cmdList->argv[0], cmdList->argv);
        if (x < 0) {
            int errno2 = errno;
            perror("execvp() error");
            exit(errno2);
        }
        // don't think this will ever actually get returned
        // just to handle warning error of non-void function
        return 0;
    }
    // parent
    else {
        // wait for child to exit
        int child_status;

        // ignore CTRL-C
        if (signal(SIGINT, SIG_IGN) == SIG_ERR) {
            int errno2 = errno;
            perror("signal() error");  
            return errno2;
        }

        waitpid(pid, &child_status, 0);

        if (signal(SIGINT, SIG_DFL) == SIG_ERR) {
            int errno2 = errno;
            perror("signal() error");  
            return errno2;
        }

        // return child_status, should be 0 on successful child process
        return STATUS(child_status);
    }
}


// '<' or '<<'
void redirect_stdin(const CMD *cmdList) {
    // handle fromType 
    switch (cmdList->fromType) {
        case NONE:
            break;
            
        // '<'
        case RED_IN:
        {
            int new_stdin_fd = open(cmdList->fromFile, O_RDONLY);
            // unsuccessful open returns -1
            if (new_stdin_fd < 0) {
                int errno2 = errno;
                perror("Open error");
                exit(errno2);
            }
            // overwrite stdin to refer to new_stdin_fd (the opened fromFile)
            dup2(new_stdin_fd, STDIN_FILENO);
            // close opened file fd
            close(new_stdin_fd);
            break;
        }
            
        // '<<'
        case RED_IN_HERE:
        {
            char tmp[] = "tmp_XXXXXX";

            // generates a unique temporary filename from tmp, creates and opens the file,
                // and returns an open fd for the file
            int new_stdin_fd = mkstemp(tmp);
            if (new_stdin_fd < 0) {
                int errno2 = errno;
                perror("Mkstemp() error");
                exit(errno2);
            }
            
            int w = write(new_stdin_fd, cmdList->fromFile, strlen(cmdList->fromFile));
            // write the contents of HERE doc to the tmp file (new_stdin_fd)
            if (w < 0) {
                int errno2 = errno;
                perror("Write error");
                exit(errno2);
            }

            // close tmp file
            close(new_stdin_fd);
            // reopen tmp file in read only mode
            int o = open(tmp, O_RDONLY);
            if (o < 0) {
                int errno2 = errno;
                perror("Open error");
                exit(errno2);
            }
            // overwrite stdin to refer to new_stdin_fd (the opened fromFile)
            dup2(new_stdin_fd, STDIN_FILENO);
            // close tmp file fd
            close(new_stdin_fd);
            break;
        }

        default:
            break;
    }

    return;
}


// '>' or '>>'
void redirect_stdout(const CMD *cmdList) {
    switch (cmdList->toType) {
        case NONE:
            break;
        
        // '>'
        case RED_OUT:
        {
            // O_RDWR = read/write permission
            // O_CREAT = if file DNE, create it
            // O_TRUNC = file truncated to length 0
            // S_IRWXU = 00700 user (file owner) has read, write, and execute permission
            int new_stdout_fd = open(cmdList->toFile, O_RDWR|O_CREAT|O_TRUNC, S_IRWXU);
            // unsuccessful open returns -1
            if (new_stdout_fd < 0) {
                int errno2 = errno;
                perror("Open error");
                exit(errno2);
            }
            // overwrite stdout to refer to new_stdout_fd (the opened toFile)
            dup2(new_stdout_fd, STDOUT_FILENO);
            // close opened file fd
            close(new_stdout_fd);
            break;
        }

        // '>>'
        case RED_OUT_APP:
        {
            // O_APPEND = file opened in append mode
                // before each write, the file offset is positioned at the end of the file
            int new_stdout_fd = open(cmdList->toFile, O_RDWR|O_CREAT|O_APPEND, S_IRWXU);
            // unsuccessful open returns -1
            if (new_stdout_fd < 0) {
                int errno2 = errno;
                perror("Open error");
                exit(errno2);
            }
            // overwrite stdout to refer to new_stdout_fd (the opened toFile)
            dup2(new_stdout_fd, STDOUT_FILENO);
            // close opened file fd
            close(new_stdout_fd);
            break;
        }
    }
    return;
}


// '|'
int pipe_command(const CMD *cmdList) {

    // pipefd[0] refers to the read end of the pipe
    // pipefd[1] refers to the write end of the pipe
    int pipefd[2];

    int p = pipe(pipefd);
    // pipe failure returns -1
    if (p < 0) {
        int errno2 = errno;
        perror("Pipe failure");
        return errno2;
    }

    // pipe left child node
    int pid_left_child = fork();

    // fork failure returns -1
    if (pid_left_child < 0) {
        int errno2 = errno;
        perror("Fork failure");
        return errno2;
    }

    // process left child node; overwrite stdout
    if (pid_left_child == 0) {
        // copied similar from pipe.c
        // close read end
        close (pipefd[0]); 
        // overwrite stdout to refer to pipefd[1] (left child's write end of pipe)
        dup2(pipefd[1], STDOUT_FILENO);
        // close write end
        close(pipefd[1]);
        // exit w/ status of recursive call because left child could be of any type
            // process() will call execvp() on cmdList->left
        exit(process(cmdList->left));
    }

    // parent of left child's fork
    else {
        
        // fork again from parent
        int pid_right_child = fork();    // pipe right child node

        // fork failure returns -1
        if (pid_right_child < 0) {
            int errno2 = errno;
            perror("Fork failure");
            return errno2;
        }

        // process right child node; overwrite stdin
        if (pid_right_child == 0) {
            // close write end
            close (pipefd[1]); 
            // overwrite stdin to refer to pipefd[0] (right child's read end of pipe)
            dup2(pipefd[0], STDIN_FILENO);
            // close read end
            close(pipefd[0]);
            // exit w/ status of recursive call because right child could be of any type
            exit(process(cmdList->right));
        }

        // parent of right child's fork
        else {
            // close pipefd's before waiting
            close(pipefd[0]);
            close(pipefd[1]);

            // wait for both children to finish
            int left_child_status;
            int right_child_status;

            if (signal(SIGINT, SIG_IGN) == SIG_ERR) {
                int errno2 = errno;
                perror("signal() error");  
                return errno2;
            }

            waitpid(pid_left_child, &left_child_status, 0);
            waitpid(pid_right_child, &right_child_status, 0);
            
            if (signal(SIGINT, SIG_DFL) == SIG_ERR) {
                int errno2 = errno;
                perror("signal() error");  
                return errno2;
            }

            int left_status = STATUS(left_child_status);
            int right_status = STATUS(right_child_status);

            // if both children successfully exited, return 0
            if (left_status == 0 && right_status == 0) {
                return 0;
            }
            // if right child unsuccessfully exits, return its status
            if (left_status == 0 && right_status != 0) {
                return right_status;
            }
            // if left child unsuccessfully exits, return its status
            if (left_status != 0 && right_status == 0) {
                return left_status;
            }
            // if both unsuccessfully exit, return latest (rightmost) stage to fail
            if (left_status != 0 && right_status != 0) {
                return right_status;
            }

        }
    }
    // don't think this will ever actually get returned
    // just to handle warning error of non-void function
    return 0;
}


// '&&'
int and_command(const CMD *cmdList) {
    // A && B; First process A. If it returns false (non-zero), skip B.
    // if left child returns true (0), process right child
    int A = process(cmdList->left);
    if (A == 0) {
        // if right child returns true (0)
        int B = process(cmdList->right);
        if (B == 0) {
            // return true (0) since both operands of && true
            return 0;
        }
        else {
            return B;
        }
    }
    return A;
}


// '||'
int or_command(const CMD *cmdList) {
    // A || B; First process A. If it returns 0 (true), skip B.
    // if left child returns false (non-zero), process right child
    int A = process(cmdList->left);
    if (A != 0) {
        // if right child also false (non-zero)
        int B = process(cmdList->right);
        if (B != 0) {
            // both operands false, return errno
            return B;
        }
    }
    // return 0 if at least one operand returns 0 (true)
    return 0;
}


int sub_command(const CMD *cmdList) {
    // similar to simple command
    int pid = fork();

    // fork failure returns -1
    if (pid < 0) {
        int errno2 = errno;
        perror("Fork failure");
        return errno2;
    }

    // child
    if (pid == 0) {
        // set local vars
        for (int i = 0; i < cmdList->nLocal; i++) {
            setenv(cmdList->locVar[i], cmdList->locVal[i], 1);
        }

        // handle redirection
        redirect_stdin(cmdList);
        redirect_stdout(cmdList);

        // In this case the subshell (simply a forked child shell) would recursively 
        // process the child command node (cmdList->left) and exit with its status
        exit(process(cmdList->left));
    }

    // parent
    else {
        // wait for child to exit
        int child_status;

        if (signal(SIGINT, SIG_IGN) == SIG_ERR) {
            int errno2 = errno;
            perror("signal() error");  
            return errno2;
        }

        waitpid(pid, &child_status, 0);

        if (signal(SIGINT, SIG_DFL) == SIG_ERR) {
            int errno2 = errno;
            perror("signal() error");  
            return errno2;
        }
        // return child_status, should be 0 on successful child process
        return STATUS(child_status);
    }
}


// ';'
int end_command(const CMD *cmdList) {
    int ret_val = process(cmdList->left);
    // if right child exists, process{}
    if (cmdList->right != NULL) {
        ret_val = process(cmdList->right);
    }
    return ret_val;
}


// '&'
int background_command(const CMD *cmdList) { 
    int left_status;
    int right_status;

    int left_left_status;
    int left_right_status;

    // check if left child is another SEP_BG (&)
    if (cmdList->left->type == SEP_BG) {
        left_left_status = background_command_helper(cmdList->left->left);
        if (cmdList->left->right != NULL) {
            left_right_status = background_command_helper(cmdList->left->right);
        }
        // prioritize leftmost (left_left_status) on error, assign left_status
        if (left_left_status != 0) {
            left_status = left_left_status;
        }
        else {
            left_status = left_right_status;
        }
    }
    // check if left child is SEP_END (;)
    if (cmdList->left->type == SEP_END) {
        left_left_status = process(cmdList->left->left);
        if (cmdList->left->right != NULL) {
            left_right_status = background_command_helper(cmdList->left->right);
        }
        if (left_left_status != 0) {
            left_status = left_left_status;
        }
        else {
            left_status = left_right_status;
        }
    }
    else {
        left_status = background_command_helper(cmdList->left);
    }

    // if right child exists, process()
    if (cmdList->right != NULL) {
        right_status = process(cmdList->right);
    }

    // priortize left over right when returning

    // if both children successfully exited, return 0
    if (left_status == 0 && right_status == 0) {
        return 0;
    }
    // if left child unsuccessfully exits, return its status
    if (left_status != 0 && right_status == 0) {
        return left_status;
    }
    // if right child unsuccessfully exits, return its status
    if (left_status == 0 && right_status != 0) {
        return right_status;
    }
    // if both unsuccessfully exit, return leftmost stage to fail
    if (left_status != 0 && right_status != 0) {
        return left_status;
    }

    // don't think this will ever actually get returned
    // just to handle warning error of non-void function
    return 0;
}


int background_command_helper(const CMD *cmdList) {
    int status;
    int left_status;
    int right_status;

    if (cmdList->type == SEP_BG) {
        left_status = background_command(cmdList->left);
        if (cmdList->right != NULL) {
            right_status = background_command(cmdList->right);
        }
        if (left_status != 0) {
            status = left_status;
        }
        else {
            status = right_status;
        }
        return status;
    }

    if (cmdList->type == SEP_END) {
        left_status = process(cmdList->left);
        if (cmdList->right != NULL) {
            right_status = background_command_helper(cmdList->right);
        }
        if (left_status != 0) {
            status = left_status;
        }
        else {
            status = right_status;
        }
        return status;
    }

    else {
        int pid = fork();

        if (pid < 0) {
            int errno2 = errno;
            perror("Fork failure");
            env_variable(pid);
            return errno2;
        }

        // child
        if (pid == 0) {
            exit(process(cmdList));
        }

        // parent
        else {
            // do not waitpid for child 
            int f = fprintf(stderr, "Backgrounded: %d\n", pid);
            if (f < 0) {
                int errno2 = errno;
                perror("fprintf() failure");
                env_variable(f);
                return errno2;
            }
        }
    }

    // don't think this will ever actually get returned
    // just to handle warning error of non-void function
    return 0;
}


void env_variable(int status) {
    // to store last command's "printed" value
    char printed_value[10];
    // convert ret_val to string and store in char array
    sprintf(printed_value, "%d", status);
    // set $? to last command's "printed" value
    setenv("?", printed_value, 1);
    return;
}


int built_in_command(const CMD *cmdList) {
    // will return 0 at end if successful through all code
    int ret_val = 0;

    // set local vars
    for (int i = 0; i < cmdList->nLocal; i++) {
        setenv(cmdList->locVar[i], cmdList->locVal[i], 1);
    }

    // handle fromType
    int x = redirect_stdin_builtin(cmdList);
    if (x != 0) {
        int errno2 = errno;
        return errno2;
    }
    // handle toType
    int y = redirect_stdout_builtin(cmdList);
    if (y < 0) {
        int errno2 = errno;
        return errno2;
    }

    // check which built-in it is and change int case accordingly for switch()
    char* command = cmdList->argv[0];
    int type = 0;

    if (strcmp(command, "cd") == 0) {
        type = 1;
    }
    if (strcmp(command, "pushd") == 0) {
        type = 2;
    }
    if (strcmp(command, "popd") == 0) {
        type = 3;
    }

    switch(type) {
        // cd
        case 1:
            // if more than 2 arguments; "cd /c/cs323 too/many"
            if (cmdList->argc > 2) {
                perror("Too many arguments");
                // return 1 if incorrect number of args
                return 1;
            }
            // just cd no directory
            if (cmdList->argc == 1) {
                // change to home directory
                int c = chdir(getenv("HOME"));
                if (c < 0) {
                    int errno2 = errno;
                    perror("chdir() error");
                    return errno2;
                }
                break;
            }
            // correct case; "cd target_directory"
            if (cmdList->argc == 2) {
                // change to directory argument
                int c = chdir(cmdList->argv[1]);
                if (c < 0) {
                    int errno2 = errno;
                    perror("chdir() error");
                    return errno2;
                }
                break;
            }
            break;

        // pushd
        case 2:
            // must have one argument
            if (cmdList->argc != 2) {
                perror("pushd arguments");
                return 1;
            }
            // correct case; "pushd target_directory"
            if (cmdList->argc == 2) {
                ret_val = pushd_command(cmdList);
                break;
            }
            break;

        // popd
        case 3:
            // can only be popd itself, so argc cannot be > 1
            if (cmdList->argc > 1) {
                perror("Too many arguments");
                return 1;
            }
            // correct case; "popd"
            if (cmdList->argc == 1) {
                ret_val = popd_command(cmdList);
                break;
            }
            break;

        default:
            break;
    }
    return ret_val;
}


int redirect_stdin_builtin(const CMD *cmdList) {
    // handle fromType 
    switch (cmdList->fromType) {
        case NONE:
            break;
            
        // '<'
        case RED_IN:
        {
            int new_stdin_fd = open(cmdList->fromFile, O_RDONLY);
            // unsuccessful open returns -1
            if (new_stdin_fd < 0) {
                int errno2 = errno;
                perror("Open error");
                return errno2;
            }
            // overwrite stdin to refer to new_stdin_fd (the opened fromFile)
            dup2(new_stdin_fd, STDIN_FILENO);
            // close opened file fd
            close(new_stdin_fd);
            break;
        }
            
        // '<<'
        case RED_IN_HERE:
        {
            char tmp[] = "tmp_XXXXXX";

            // generates a unique temporary filename from tmp, creates and opens the file,
                // and returns an open fd for the file
            int new_stdin_fd = mkstemp(tmp);
            if (new_stdin_fd < 0) {
                int errno2 = errno;
                perror("Mkstemp() error");
                return errno2;
            }
            
            int w = write(new_stdin_fd, cmdList->fromFile, strlen(cmdList->fromFile));
            // write the contents of HERE doc to the tmp file (new_stdin_fd)
            if (w < 0) {
                int errno2 = errno;
                perror("Write error");
                return errno2;
            }

            // close tmp file
            close(new_stdin_fd);
            // reopen tmp file in read only mode
            int o = open(tmp, O_RDONLY);
            if (o < 0) {
                int errno2 = errno;
                perror("Open error");
                return errno2;
            }
            // overwrite stdin to refer to new_stdin_fd (the opened fromFile)
            dup2(new_stdin_fd, STDIN_FILENO);
            // close tmp file fd
            close(new_stdin_fd);
            break;
        }

        default:
            break;
    }

    return 0;
}


int redirect_stdout_builtin(const CMD *cmdList) {
    switch (cmdList->toType) {
        case NONE:
            break;
        
        // '>'
        case RED_OUT:
        {
            // O_RDWR = read/write permission
            // O_CREAT = if file DNE, create it
            // O_TRUNC = file truncated to length 0
            // S_IRWXU = 00700 user (file owner) has read, write, and execute permission
            int new_stdout_fd = open(cmdList->toFile, O_RDWR|O_CREAT|O_TRUNC, S_IRWXU);
            // unsuccessful open returns -1
            if (new_stdout_fd < 0) {
                int errno2 = errno;
                perror("Open error");
                return errno2;
            }
            // overwrite stdout to refer to new_stdout_fd (the opened toFile)
            dup2(new_stdout_fd, STDOUT_FILENO);
            // close opened file fd
            close(new_stdout_fd);
            break;
        }

        // '>>'
        case RED_OUT_APP:
        {
            // O_APPEND = file opened in append mode
                // before each write, the file offset is positioned at the end of the file
            int new_stdout_fd = open(cmdList->toFile, O_RDWR|O_CREAT|O_APPEND, S_IRWXU);
            // unsuccessful open returns -1
            if (new_stdout_fd < 0) {
                int errno2 = errno;
                perror("Open error");
                return errno2;
            }
            // overwrite stdout to refer to new_stdout_fd (the opened toFile)
            dup2(new_stdout_fd, STDOUT_FILENO);
            // close opened file fd
            close(new_stdout_fd);
            break;
        }
    }
    return 0;
}

int pushd_command(const CMD *cmdList) {
    // set node
    Node *node = malloc(sizeof(Node));
    node->directory = malloc(sizeof(char) * 256);
    // store current directory to popd back to
    node->directory = getcwd(node->directory, 256);
    // getcwd() returns NULL on failure
    if (node->directory == NULL) {
        int errno2 = errno;
        perror("getcwd() error");
        return errno2;
    }
    // make new node point to head of current linked list
    node->next = head;
    // move head of llist to newly pushed node
    head = node;
    
    // chdir to argv[1] target_directory
    int c = chdir(cmdList->argv[1]);
    if (c < 0) {
        int errno2 = errno;
        perror("chdir() error");
        return errno2;
    }

    char *current_directory = get_current_dir_name();
    if (current_directory == NULL) {
        int errno2 = errno;
        perror("getcwd() error");
        return errno2;
    }

    //  print "current_directory (all paths pushed onto stack)""
    int p = printf("%s", current_directory);
    if (p < 0) {
        int errno2 = errno;
        perror("printf() error");
        return errno2;
    }
    free(current_directory);

    // print each dir on stack
    Node *tmp = head;
    while (tmp != NULL) {
        int x = printf(" %s", tmp->directory);
        if (x < 0) {
            int errno2 = errno;
            perror("printf() error");
            return errno2;
        }
        tmp = tmp->next;
    }

    // print a newline
    int y = printf("\n");
    if (y < 0) {
        int errno2 = errno;
        perror("printf() error");
        return errno2;
    }


    return 0;
}


int popd_command(const CMD *cmdList) {
    
    if (head == NULL) {
        int errno2 = errno;
        perror("Stack empty");
        return errno2;
    }

    // prints all directorys on stack
    Node *tmp = head;
    while (tmp != NULL) {
        int x = printf("%s ", tmp->directory);
        if (x < 0) {
            int errno2 = errno;
            perror("printf() error");
            return errno2;
        }
        tmp = tmp->next;
    }

    // print a newline
    int y = printf("\n");
    if (y < 0) {
        int errno2 = errno;
        perror("printf() error");
        return errno2;
    }

    // chdir to directory at top of stack
    Node *popped_node = head;
    head = head->next;
    int c = chdir(popped_node->directory);
    if (c < 0) {
        int errno2 = errno;
        perror("chdir() error");
        return errno2;
    }
    free(popped_node->directory);
    free(popped_node);

    return 0;
}