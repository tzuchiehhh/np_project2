#include <csignal>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <queue>
#include <sstream>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wait.h>

#define BUFFER_SIZE 15001

using namespace std;

struct Command {
    vector<string> exec_command;
    bool is_error_pipe = false;
    // -1: no pipe after this command; 0:oridinary pipe; >0: number of commands need to be jumped
    int pipe_number;
    pair<int, int> input_pipe;
    pair<int, int> output_pipe;

    bool file_redirection = false;
    string filename;
};

struct Job {
    int job_id;
    bool is_built_in_command = false;
    vector<string> built_in_command;
    queue<Command> command_queue;
};

pair<int, int> create_pipe() {
    // pipefd[0]: read, pipefd[1]: write
    // create pipe
    int pipefd[2];
    int create_pipe = pipe(pipefd);
    // create pipe failed
    while (create_pipe < 0) {
        create_pipe = pipe(pipefd);
    }

    pair<int, int> temp;
    temp.first = pipefd[0];
    temp.second = pipefd[1];
    return temp;
}

pair<int, int> create_numbered_pipe(int des_job_id, map<int, pair<int, int>> &numbered_pipe_map) {
    pair<int, int> temp;
    if ((numbered_pipe_map.find(des_job_id) != numbered_pipe_map.end())) {
        temp.first = numbered_pipe_map[des_job_id].first;
        temp.second = numbered_pipe_map[des_job_id].second;
    } else {
        temp = create_pipe();
    }
    numbered_pipe_map[des_job_id] = temp;
    return temp;
}

void open_pipe(int in, int out) {
    dup2(in, STDIN_FILENO);
    dup2(out, STDOUT_FILENO);
    return;
}

void close_pipe(pair<int, int> p) {
    close(p.first);
    close(p.second);
    return;
}

bool handle_built_in_command(vector<string> tokens, int client_socket) {
    // Terminate client connection
    bool terminate_client = false;
    if (tokens[0] == "exit") {
        terminate_client = true;
        // exit(0);
    } else if (tokens[0] == "setenv") {
        setenv(tokens[1].c_str(), tokens[2].c_str(), 1);
    } else if (tokens[0] == "printenv") {
        const char *environment_variable = getenv(tokens[1].c_str());
        // check the variable exists or not
        if (environment_variable) {
            // cout << environment_variable << endl;
            string send_environment_variable = (string)environment_variable + "\n";
            send(client_socket, send_environment_variable.c_str(), strlen(send_environment_variable.c_str()), 0);
        }
    }
    return terminate_client;
}

int parse(queue<Job> &job_queue, const string &str, const char &delimiter, int job_id) {

    queue<Command> command_queue;
    Job job;
    Command command;
    vector<string> exec_command;
    stringstream ss(str);
    string token;
    while (getline(ss, token, delimiter)) {

        // built-in command arguments
        if (job.is_built_in_command) {
            job.built_in_command.push_back(token);
        }
        // First built-in command
        else if (token == "setenv" || token == "printenv" || token == "exit") {
            job.is_built_in_command = true;
            job.built_in_command.push_back(token);

        }
        // pipe or numbered pipe
        else if (token[0] == '|' || (token[0] == '!' && token.length() > 1)) {
            // oridinary pipe = 0
            if (token == "|") {

                command.pipe_number = 0;
                command_queue.push(command);
                command = Command();
            }
            // numbered pipe
            else if (token[0] == '|') {

                const char *t = token.c_str();
                command.pipe_number = atoi(t + 1);
                command_queue.push(command);
                command = Command();
                job_id++;
                job.job_id = job_id;
                job.command_queue = command_queue;
                command_queue = queue<Command>();
                job_queue.push(job);
                job = Job();

            }
            // error pipe
            else if (token[0] == '!') {

                const char *t = token.c_str();
                command.pipe_number = atoi(t + 1);
                command.is_error_pipe = true;
                command_queue.push(command);
                command = Command();

                job_id++;
                job.job_id = job_id;
                job.command_queue = command_queue;
                command_queue = queue<Command>();
                job_queue.push(job);
                job = Job();
            }
        }
        // get filename
        else if (command.file_redirection) {
            command.filename = token;
        }
        // file redirection
        else if (token == ">") {
            command.file_redirection = true;
        }
        // other commands
        else {
            command.exec_command.push_back(token);
        }
    }

    if (job.is_built_in_command) {
        job_id++;
        job.job_id = job_id;
        job_queue.push(job);
        job = Job();
    }
    // last command
    else if (command.exec_command.size() != 0) {
        command.pipe_number = -1; // no pipe
        command_queue.push(command);
        command = Command();
        job_id++;
        // cout << "job_id = " << job_id << endl;
        job.job_id = job_id;
        job.command_queue = command_queue;
        // cout << command_queue.size() << endl;
        command_queue = queue<Command>();
        job_queue.push(job);
        job = Job();
    }

    // while (!job_queue.empty()) {
    //     Job c_job = job_queue.front();
    //     cout << "job id = " << c_job.job_id << endl;
    //     ;
    //     while (!c_job.command_queue.empty()) {
    //         Command c_command = c_job.command_queue.front();
    //         cout<<"exec command= ";
    //         for(int i=0; i<c_command.exec_command.size();i++)
    //             cout<<c_command.exec_command[i]<<" ";

    //         cout << " ; pipe number = " << c_command.pipe_number << endl;
    //         c_job.command_queue.pop();
    //     }
    //     job_queue.pop();

    // }
    return job_id;
}

char **to_char_array(vector<string> input) {
    char **args;
    args = new char *[input.size() + 1];
    for (int i = 0; i < input.size(); i++) {
        args[i] = strdup((input[i]).c_str());
    }
    args[input.size()] = NULL;
    return args;
}

void execute(Command command, map<int, pair<int, int>> &numbered_pipe_map, int client_socket) {

    // fork process
    pid_t pid;
    while (1) {
        pid = fork();
        if (pid >= 0) {
            break;
        }
    }

    // child process
    if (pid == 0) {
        // cout << "child process" << endl;
        dup2(client_socket, STDOUT_FILENO); // Redirect stdout to client_socket
        dup2(client_socket, STDERR_FILENO); // Redirect stderr to client_socket

        // some data pass to this command through pipe
        if (command.input_pipe.first != 0) {
            dup2(command.input_pipe.first, STDIN_FILENO);
            close_pipe(command.input_pipe);
        }

        // pipe after this command
        if (command.output_pipe.second != 0) {
            // if there is a pipe for stderr after this command, duplicate the file descriptor
            if (command.is_error_pipe)
                dup2(command.output_pipe.second, STDERR_FILENO);
            dup2(command.output_pipe.second, STDOUT_FILENO);
            close_pipe(command.output_pipe);
        }

        // deallocate the file descriptors stored in numbered_pipe_map
        for (map<int, pair<int, int>>::iterator it = numbered_pipe_map.begin(); it != numbered_pipe_map.end(); ++it) {

            close_pipe(it->second);
        }

        // exec
        bool redireciton = false;
        string file_name = "";

        char **args = to_char_array(command.exec_command);

        if (command.file_redirection) {
            freopen(command.filename.c_str(), "w", stdout);
        }

        if (execvp(args[0], args) == -1) {

            cerr << "Unknown command: [" << args[0] << "]." << endl;
            exit(0);
        }

    }
    // parent process
    else {
        //  if there is a input pipe, deallocate the file descriptor
        if (command.input_pipe.first != 0) {
            close_pipe(command.input_pipe);
        }

        // if there is any pipe (including ordinary pipe and numbered pipe) after the command, don't wait
        if (command.pipe_number >= 0) {
            signal(SIGCHLD, SIG_IGN);

        } else {

            wait(NULL);
        }
    }

    return;
}

int main(int argc, const char *argv[]) {

    int server_port = stoi(argv[1], nullptr);
    // set initial environment variable
    setenv("PATH", "bin:.", 1);

    int server_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    // Create socket
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        cerr << "Socket creation failed" << endl;
        exit(-1);
    }

    // let address reuse
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(int));

    bzero(&server_addr, sizeof(server_addr));
    // TCP server address and port
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;  // Bind to any available interface
    server_addr.sin_port = htons(server_port); // Port number

    // bind socket to address
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        cerr << "bind socket failed" << endl;
        exit(-1);
    }

    // listen
    if (listen(server_socket, 1) < 0) {
        cerr << "server: listen socket failed" << endl;
        exit(-1);
    }
    string user_input;
    map<int, pair<int, int>> numbered_pipe_map;
    int job_id = 0;
    while (1) {

        // accept
        int client_socket;
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, (socklen_t *)&client_len);
        if (client_socket < 0) {
            cerr << "server accept error" << endl;
            continue;
        }

        bool terminate_client = false;
        while (!terminate_client) {

            const char *prompt_str = "% ";
            // send % to client
            send(client_socket, prompt_str, strlen(prompt_str), 0);

            // Receive user input from client
            char buffer[BUFFER_SIZE];
            int bytes_received = read(client_socket, buffer, BUFFER_SIZE);
            // cout << bytes_received << endl;
            if (bytes_received <= 0) {
                cerr << "Error receiving data from client" << endl;
                close(client_socket);
                continue;
            }

            // replace newline character with null character
            char *bp;
            if ((bp = strchr(buffer, '\n')) != NULL)
                *bp = '\0';
            if ((bp = strchr(buffer, '\r')) != NULL)
                *bp = '\0';
            user_input = (string)(buffer);
            // cout << "user input: " << user_input << endl;
            // parse user input
            char delimiter = ' ';
            queue<Job> job_queue;
            job_id = parse(job_queue, user_input, delimiter, job_id);
            while (!job_queue.empty()) {
                Job c_job = job_queue.front();
                if (c_job.is_built_in_command) {
                    terminate_client = handle_built_in_command(c_job.built_in_command, client_socket);
                    if (terminate_client) {
                        break;
                    }
                    job_queue.pop();
                    continue;
                }
                // cout << "command queue size: " << c_job.command_queue.size() << endl;
                while (!c_job.command_queue.empty()) {
                    Command c_command = c_job.command_queue.front();
                    c_job.command_queue.pop();

                    // cout << "current execute command: ";
                    // for (int i = 0; i < c_command.exec_command.size(); i++) {
                    //     cout << c_command.exec_command[i] << " ";
                    // }
                    // cout << endl;

                    // oridinary pipe after this command
                    if (c_command.pipe_number == 0) {

                        // read next command
                        Command *n_command = &c_job.command_queue.front();

                        // create pipe and assign it to current command output pipe
                        c_command.output_pipe = create_pipe();

                        // assign current command output pipe to next command input pipe
                        n_command->input_pipe = c_command.output_pipe;
                    }
                    // numbered pipe
                    else if (c_command.pipe_number > 0) {
                        c_command.output_pipe = create_numbered_pipe(c_job.job_id + c_command.pipe_number, numbered_pipe_map);
                    }

                    // if there exists numbered piped which send data to this command
                    if (numbered_pipe_map.find(c_job.job_id) != numbered_pipe_map.end()) {

                        c_command.input_pipe = numbered_pipe_map[c_job.job_id];
                        numbered_pipe_map.erase(c_job.job_id);
                    }

                    // if no pipe after this command, execute directly
                    execute(c_command, numbered_pipe_map, client_socket);
                }

                job_queue.pop();
            }
        }
        close(client_socket);
    }

    // close socket
    close(server_socket);
    return 0;
}