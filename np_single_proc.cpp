#include <arpa/inet.h>
#include <csignal>
#include <iostream>
#include <map>
#include <netdb.h>
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
// void BroadCast(string message);
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

struct Client {

    queue<Job> job_queue;
    int id = -1;
    int conn_fd = -1;
    string ip;
    int port;
    string nickname = "(no name)";
    char buffer[BUFFER_SIZE];
    map<int, pair<int, int>> numbered_pipe_map;

    map<string, string> environment_var_map;

} clients[31];

void send_msg(int fd, string message, bool boradcast = false) {
    if (boradcast) {
        for (int i = 1; i < 31; i++) {
            if (clients[i].conn_fd != -1)
                send(clients[i].conn_fd, message.c_str(), strlen(message.c_str()), 0);
        }

    } else {
        send(fd, message.c_str(), strlen(message.c_str()), 0);
    }
    return;
}

void client_login(Client &client) {
    string welcome_msg = "****************************************\n** Welcome to the information server. **\n****************************************\n";
    string notify_msg = "*** User \'" + client.nickname + "\' entered from " + string(client.ip) + ":" + to_string(client.port) + ". ***\n";
    // welcome message
    send_msg(client.conn_fd, welcome_msg);
    // broadcast message
    send_msg(-1, notify_msg, true);
    client.environment_var_map["PATH"] = "bin:.";
};

void init_client_env(Client client) {
    for (map<string, string>::iterator it = client.environment_var_map.begin(); it != client.environment_var_map.end(); ++it) {
        // cout << (it->first) << " " << (it->second) << endl;
        setenv(const_cast<char *>(it->first.c_str()), const_cast<char *>(it->second.c_str()), 1);
    }
};

void unset_client_env(Client client) {
    for (map<string, string>::iterator it = client.environment_var_map.begin(); it != client.environment_var_map.end(); ++it)
        unsetenv(it->first.c_str());
};

string remove_endl(char *s) {
    char *bp;
    if ((bp = strchr(s, '\n')) != NULL)
        *bp = '\0';
    if ((bp = strchr(s, '\r')) != NULL)
        *bp = '\0';

    return (string)(s);
}

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

int passivesock(const char *service, const char *protocol, int qlen) {
    struct servent *pse;        // pointer to service information entry
    struct protoent *ppe;       // pointer to protocol information entry
    struct sockaddr_in sin;     // an Internrt endpoint address
    int socket_fd, socket_type; // socket descriptor and socket type

    bzero((char *)&sin, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;

    // map service name to port number
    if (pse = getservbyname(service, protocol))
        sin.sin_port = htons(ntohs((u_short)pse->s_port));
    else if ((sin.sin_port = htons((u_short)atoi(service))) == 0) {
        exit(-1);
    }

    // map protocol name to protocol number
    if ((ppe = getprotobyname(protocol)) == 0) {
        exit(-1);
    }

    // use protocol to choose a socket type
    if (strcmp(protocol, "tcp") == 0)
        socket_type = SOCK_STREAM;
    else
        socket_type = SOCK_DGRAM;

    // allocate a socket
    socket_fd = socket(PF_INET, socket_type, ppe->p_proto);
    if (socket_fd < 0) {
        exit(-1);
    }
    const int enable = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));

    // bind the socket
    if (bind(socket_fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        exit(-1);
    }
    if (socket_type == SOCK_STREAM && listen(socket_fd, qlen) < 0) {
        exit(-1);
    }
    return socket_fd;
}

int passiveTCP(const char *service, int qlen) {
    return passivesock(service, "tcp", qlen);
}

void process_client_command(Client client) {
    // bool terminate_client = false;
    // while (!terminate_client) {

    //     const char *prompt_str = "% ";
    //     // send % to client
    //     send(client.conn_fd, prompt_str, strlen(prompt_str), 0);

    //     // Receive user input from client
    //     char buffer[BUFFER_SIZE];
    //     int bytes_received = read(client_socket, buffer, BUFFER_SIZE);
    //     // cout << bytes_received << endl;
    //     if (bytes_received <= 0) {
    //         cerr << "Error receiving data from client" << endl;
    //         close(client_socket);
    //         continue;
    //     }

    //     // replace newline character with null character
    //     char *bp;
    //     if ((bp = strchr(buffer, '\n')) != NULL)
    //         *bp = '\0';
    //     if ((bp = strchr(buffer, '\r')) != NULL)
    //         *bp = '\0';
    //     user_input = (string)(buffer);
    //     // cout << "user input: " << user_input << endl;
    //     // parse user input
    //     char delimiter = ' ';
    //     queue<Job> job_queue;
    //     job_id = parse(job_queue, user_input, delimiter, job_id);
    //     while (!job_queue.empty()) {
    //         Job c_job = job_queue.front();
    //         if (c_job.is_built_in_command) {
    //             terminate_client = handle_built_in_command(c_job.built_in_command, client_socket);
    //             if (terminate_client) {
    //                 break;
    //             }
    //             job_queue.pop();
    //             continue;
    //         }
    //         // cout << "command queue size: " << c_job.command_queue.size() << endl;
    //         while (!c_job.command_queue.empty()) {
    //             Command c_command = c_job.command_queue.front();
    //             c_job.command_queue.pop();

    //             // cout << "current execute command: ";
    //             // for (int i = 0; i < c_command.exec_command.size(); i++) {
    //             //     cout << c_command.exec_command[i] << " ";
    //             // }
    //             // cout << endl;

    //             // oridinary pipe after this command
    //             if (c_command.pipe_number == 0) {

    //                 // read next command
    //                 Command *n_command = &c_job.command_queue.front();

    //                 // create pipe and assign it to current command output pipe
    //                 c_command.output_pipe = create_pipe();

    //                 // assign current command output pipe to next command input pipe
    //                 n_command->input_pipe = c_command.output_pipe;
    //             }
    //             // numbered pipe
    //             else if (c_command.pipe_number > 0) {
    //                 c_command.output_pipe = create_numbered_pipe(c_job.job_id + c_command.pipe_number, numbered_pipe_map);
    //             }

    //             // if there exists numbered piped which send data to this command
    //             if (numbered_pipe_map.find(c_job.job_id) != numbered_pipe_map.end()) {

    //                 c_command.input_pipe = numbered_pipe_map[c_job.job_id];
    //                 numbered_pipe_map.erase(c_job.job_id);
    //             }

    //             // if no pipe after this command, execute directly
    //             execute(c_command, numbered_pipe_map, client_socket);
    //         }

    //         job_queue.pop();
    //     }
    // }
    // close(client_socket);
}

// Client clients[31];
int main(int argc, const char *argv[]) {

    // set initial environment variable
    setenv("PATH", "bin:.", 1);

    int server_port = stoi(argv[1], nullptr);
    // set service to server port
    char *service = (char *)argv[1];
    // master server socket
    int msock = passiveTCP(service, 30);

    fd_set rfds;    // read file descriptor set
    fd_set afds;    // active file descriptor set
    FD_ZERO(&afds); // clear afds
    FD_ZERO(&rfds); // clear rfds

    // add msock into afds
    FD_SET(msock, &afds);

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    string user_input;
    map<int, pair<int, int>> numbered_pipe_map;
    int job_id = 0;
    while (1) {

        // copy afds to rfds
        memcpy(&rfds, &afds, sizeof(afds));
        // if (readyfd = select(maxfd + 1, &rfds, NULL, NULL, NULL) == -1) {
        //     continue;
        // }

        // get the size of file descriptor table
        int nfds = getdtablesize();
        if (select(nfds, &rfds, (fd_set *)0, (fd_set *)0, (struct timeval *)0) < 0) {
            // cerr << "select failed" << endl;
            continue;
        }

        int conn_fd;

        if (FD_ISSET(msock, &rfds)) {
            conn_fd = accept(msock, (struct sockaddr *)&client_addr, (socklen_t *)&client_len);
            while (conn_fd < 0) {
                conn_fd = accept(msock, (struct sockaddr *)&client_addr, (socklen_t *)&client_len);
            }

            // get client id
            bool find_id = false;
            for (int i = 1; i < 31; i++) {
                if (clients[i].id == -1) {
                    find_id = true;
                    clients[i].id = i;
                    clients[i].conn_fd = conn_fd;
                    clients[i].ip = string(inet_ntoa(client_addr.sin_addr));
                    clients[i].port = ntohs(client_addr.sin_port);
                    cout << "map size before login: " << clients[i].environment_var_map.size() << endl;

                    client_login(clients[i]);
                    cout << "map size after login: " << clients[i].environment_var_map.size() << endl;
                    FD_SET(conn_fd, &afds);
                    // send % to client
                    send_msg(clients[i].conn_fd, "% ");
                    break;
                }
            }

            // too many clients
            if (!find_id) {
                close(conn_fd);
            }
        }

        for (int i = 1; i < 31; i++) {
            // client does not exist
            if (clients[i].conn_fd == -1)
                continue;

            if (FD_ISSET(clients[i].conn_fd, &rfds)) {
                int readcount = read(clients[i].conn_fd, clients[i].buffer, BUFFER_SIZE);
                // get eof from client
                if (readcount == 0) {
                    // client logout
                    cout << "readcount == 0:   " << clients[i].conn_fd << endl;
                    close(clients[i].conn_fd);
                    FD_CLR(clients[i].conn_fd, &afds);

                    string msg = "*** User \'" + clients[i].nickname + "\' left. ***\n";
                    // BroadCast(msg);
                    send_msg(-1, msg, true);
                    // reset client
                    clients[i] = Client();
                } else if (readcount > 0) {
                    cout << "readcount > 0:   " << clients[i].conn_fd << endl;

                    // set current client environment variable

                    init_client_env(clients[i]);

                    // process current client command
                    process_client_command(clients[i]);

                    // unset current client environment variable
                    unset_client_env(clients[i]);

                    // clear buffer
                    memset(clients[i].buffer, '\0', sizeof(clients[i].buffer));
                    send_msg(clients[i].conn_fd, "% ");
                }
            }
        }
    }

    // close socket
    close(msock);
    return 0;
}