#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
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
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wait.h>


#define BUFFER_SIZE 15001

using namespace std;

fd_set rfds; // read file descriptor set
fd_set afds; // active file descriptor set

struct Command {
    vector<string> exec_command;
    bool is_error_pipe = false;
    // -1: no pipe after this command; 0:oridinary pipe; >0: number of commands need to be jumped
    int pipe_number;
    pair<int, int> input_pipe;
    pair<int, int> output_pipe;
    // -2: user pipe with error; -1: no user pipe >0 destination user id (the key of user_pipe_map)
    int user_pipe_input = -1;
    int user_pipe_output = -1;

    bool file_redirection = false;
    string filename;
};

struct Job {
    int job_id;
    bool is_built_in_command = false;
    bool is_self_defined_command = false;
    vector<string> built_in_command;
    vector<string> self_defined_command;
    queue<Command> command_queue;
    // string whole_command = "";
};

struct Client {

    queue<Job> job_queue;
    // string user_input = "";
    int max_job_id = 0;
    int id = -1;
    int conn_fd = -1;
    string ip;
    int port;
    string nickname = "(no name)";
    char buffer[BUFFER_SIZE];
    map<int, pair<int, int>> numbered_pipe_map;
    map<int, pair<int, int>> user_pipe_map;

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

void init_client_env(Client client) {
    for (map<string, string>::iterator it = client.environment_var_map.begin(); it != client.environment_var_map.end(); ++it) {
        // cout << (it->first) << " " << (it->second) << endl;
        setenv(const_cast<char *>(it->first.c_str()), const_cast<char *>(it->second.c_str()), 1);
    }
};

void unset_client_env(Client &client) {
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

void client_login(Client &client) {
    string welcome_msg = "****************************************\n** Welcome to the information server. **\n****************************************\n";
    string notify_msg = "*** User \'" + client.nickname + "\' entered from " + string(client.ip) + ":" + to_string(client.port) + ". ***\n";
    // welcome message
    send_msg(client.conn_fd, welcome_msg);
    // broadcast message
    send_msg(-1, notify_msg, true);
    client.environment_var_map["PATH"] = "bin:.";
};

void client_logout(Client &client) {
    string notify_msg = "*** User \'" + client.nickname + "\' left. ***\n";
    // // broadcast message
    // send_msg(-1, notify_msg, true);
    // reset buffer
    memset(client.buffer, '\0', sizeof(client.buffer));

    // close related pipe
    for (int i = 1; i < 31; i++) {
        if ((clients[i].user_pipe_map.find(client.id) != clients[i].user_pipe_map.end())) {
            close_pipe(clients[i].user_pipe_map[client.id]);
            clients[i].user_pipe_map.erase(client.id);
        }
    }
    for (int i = 1; i < 31; i++) {
        if (clients[client.id].user_pipe_map.find(i) != clients[client.id].user_pipe_map.end()) {
            close_pipe(clients[client.id].user_pipe_map[i]);
        }
    }

    // close client file descriptor
    close(client.conn_fd);
    // unset client environment variable
    unset_client_env(client);
    // clear client fd
    FD_CLR(client.conn_fd, &afds);
    // reset client
    client = Client();
    // broadcast message
    send_msg(-1, notify_msg, true);
};

bool handle_built_in_command(vector<string> tokens, Client &client) {
    // Terminate client connection
    bool terminate_client = false;
    if (tokens[0] == "exit") {
        terminate_client = true;
        // exit(0);
    } else if (tokens[0] == "setenv") {
        setenv(tokens[1].c_str(), tokens[2].c_str(), 1);
        client.environment_var_map[tokens[1]] = tokens[2];
    } else if (tokens[0] == "printenv") {
        const char *environment_variable = getenv(tokens[1].c_str());
        // check the variable exists or not
        if (environment_variable) {
            // cout << environment_variable << endl;
            string send_environment_variable = (string)environment_variable + "\n";
            send_msg(client.conn_fd, send_environment_variable);
            // send(client_socket, send_environment_variable.c_str(), strlen(send_environment_variable.c_str()), 0);
        }
    }
    return terminate_client;
}

void handle_self_defined_command(vector<string> tokens, Client &client) {
    if (tokens[0] == "who") {
        string msg = "<ID>\t<nickname>\t<IP:port>\t<indicate me>\n";
        for (int i = 1; i < 31; i++) {
            if (clients[i].conn_fd == -1)
                continue;

            msg += to_string(clients[i].id) + "\t" + clients[i].nickname + "\t" + string(clients[i].ip) + ":" + to_string(clients[i].port);
            if (i == client.id)
                msg += "\t<-me\n";
            else
                msg += "\n";
        }
        send_msg(client.conn_fd, msg);

    } else if (tokens[0] == "tell") {
        int user_id = atoi(tokens[1].c_str());
        // user does not exist
        if (clients[user_id].id == -1) {
            string err_msg = "*** Error: user #" + tokens[1] + " does not exist yet. ***\n";
            send_msg(client.conn_fd, err_msg);
        } else {
            string msg = "*** " + client.nickname + " told you ***:";
            for (int i = 2; i < tokens.size(); i++) {
                msg += " " + tokens[i];
            }
            msg += "\n";
            send_msg(clients[user_id].conn_fd, msg);
        }

    } else if (tokens[0] == "yell") {
        string msg = "*** " + client.nickname + " yelled ***:";
        for (int i = 1; i < tokens.size(); i++) {
            msg += " " + tokens[i];
        }
        msg += "\n";
        send_msg(-1, msg, true);

    } else if (tokens[0] == "name") {
        for (int i = 1; i < 31; i++) {
            if (clients[i].nickname == tokens[1]) {
                string error_msg = "*** User \'" + tokens[1] + "\' already exists. ***\n";
                send_msg(client.conn_fd, error_msg);
                return;
            }
        }
        client.nickname = tokens[1];
        string msg = "*** User from " + string(client.ip) + ":" + to_string(client.port) + " is named \'" + tokens[1] + "\'. ***\n";
        send_msg(-1, msg, true);
    }
}

int parse(queue<Job> &job_queue, const string &str, const char &delimiter, int job_id) {

    queue<Command> command_queue;
    Job job;
    Command command;
    vector<string> exec_command;
    stringstream ss(str);
    string token;
    while (getline(ss, token, delimiter)) {

        // job.whole_command += " " + token;

        // built-in command arguments
        if (job.is_built_in_command) {
            job.built_in_command.push_back(token);
        } else if (job.is_self_defined_command) {
            job.self_defined_command.push_back(token);
        }
        // First built-in command
        else if (token == "setenv" || token == "printenv" || token == "exit") {
            job.is_built_in_command = true;
            job.built_in_command.push_back(token);

        } else if (token == "who" || token == "tell" || token == "yell" || token == "name") {
            job.is_self_defined_command = true;
            job.self_defined_command.push_back(token);

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
        // output user pipe : pipe to another user
        else if (token[0] == '>' && token.length() > 1) {
            const char *t = token.c_str();
            command.user_pipe_output = atoi(t + 1);
        }
        // input user pipe: get result from another user
        else if (token[0] == '<') {
            const char *t = token.c_str();
            command.user_pipe_input = atoi(t + 1);
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

    if (job.is_built_in_command || job.is_self_defined_command) {
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

void execute(Command command, Client &client) {

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
        dup2(client.conn_fd, STDOUT_FILENO); // Redirect stdout to client_socket
        dup2(client.conn_fd, STDERR_FILENO); // Redirect stderr to client_socket

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

        // user pipe input but failed
        if (command.user_pipe_input == -2) {
            freopen("/dev/null", "r", stdin);
        }

        // user pipe output but failed
        if (command.user_pipe_output == -2) {
            freopen("/dev/null", "w", stdout);
        }
        // deallocate the file descriptors stored in numbered_pipe_map
        for (map<int, pair<int, int>>::iterator it = client.numbered_pipe_map.begin(); it != client.numbered_pipe_map.end(); ++it) {
            close_pipe(it->second);
        }

        // deallocate the file descriptors stored in user_pipe_map
        for (map<int, pair<int, int>>::iterator it = client.user_pipe_map.begin(); it != client.user_pipe_map.end(); ++it) {
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

        // if there is any pipe (including ordinary pipe, numbered pipe and user pipe) after the command, don't wait
        if (command.pipe_number >= 0 || command.user_pipe_output > 0) {
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

bool process_client_command(Client &client) {

    string user_input = (string)(remove_endl(client.buffer));
    cout << to_string(client.id) << ": " << user_input << endl;
    // parse user input
    char delimiter = ' ';
    queue<Job> job_queue;

    client.max_job_id = parse(job_queue, user_input, delimiter, client.max_job_id);
    while (!job_queue.empty()) {
        Job c_job = job_queue.front();
        if (c_job.is_built_in_command) {
            if (handle_built_in_command(c_job.built_in_command, client)) {
                // client exit
                return true;
            }
            job_queue.pop();
            continue;
        }
        if (c_job.is_self_defined_command) {
            handle_self_defined_command(c_job.self_defined_command, client);
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
                c_command.output_pipe = create_numbered_pipe(c_job.job_id + c_command.pipe_number, client.numbered_pipe_map);
            }

            // if there exists numbered piped which send data to this command
            if (client.numbered_pipe_map.find(c_job.job_id) != client.numbered_pipe_map.end()) {

                c_command.input_pipe = client.numbered_pipe_map[c_job.job_id];
                client.numbered_pipe_map.erase(c_job.job_id);
            }

            // this command need to get input from the other client
            if (c_command.user_pipe_input != -1) {

                // check user exits or not
                if (c_command.user_pipe_input > 31 || clients[c_command.user_pipe_input].id == -1) {
                    string err_msg = "*** Error: user #" + to_string(c_command.user_pipe_input) + " does not exist yet. ***\n";
                    c_command.user_pipe_input = -2;
                    send_msg(client.conn_fd, err_msg);
                }
                // check pipe exists or not (user_pipe_input --> client.id)
                else if (clients[c_command.user_pipe_input].user_pipe_map.find(client.id) != clients[c_command.user_pipe_input].user_pipe_map.end()) {

                    string msg = "*** " + client.nickname + " (#" + to_string(client.id) + ") just received from " + clients[c_command.user_pipe_input].nickname + " (#" + to_string(clients[c_command.user_pipe_input].id) + ") by \'" + user_input + "\' ***\n";
                    send_msg(-1, msg, true);
                    // cout<<"before command input pipe: "<<c_command.input_pipe.first<<" "<<c_command.input_pipe.second<<endl;

                    c_command.input_pipe = clients[c_command.user_pipe_input].user_pipe_map[client.id];
                    // cout<<"after command input pipe: "<<c_command.input_pipe.first<<" "<<c_command.input_pipe.second<<endl;
                    clients[c_command.user_pipe_input].user_pipe_map.erase(client.id);

                } else {
                    string err_msg = "*** Error: the pipe #" + to_string(c_command.user_pipe_input) + "->#" + to_string(client.id) + " does not exist yet. ***\n";
                    c_command.user_pipe_input = -2;
                    send_msg(client.conn_fd, err_msg);
                }
            }

            // this command need to pipe result to another client
            if (c_command.user_pipe_output != -1) {
                // check user exits or not
                if (c_command.user_pipe_output > 31 || clients[c_command.user_pipe_output].id == -1) {
                    string err_msg = "*** Error: user #" + to_string(c_command.user_pipe_output) + " does not exist yet. ***\n";
                    c_command.user_pipe_output = -2;
                    send_msg(client.conn_fd, err_msg);

                    // pipe already exists in user_pipe_map
                } else if (client.user_pipe_map.find(c_command.user_pipe_output) != client.user_pipe_map.end()) {
                    string err_msg = "*** Error: the pipe #" + to_string(client.id) + "->#" + to_string(c_command.user_pipe_output) + " already exists. ***\n";
                    c_command.user_pipe_output = -2;
                    send_msg(client.conn_fd, err_msg);

                } else {
                    // cout<<"before command output pipe: "<<c_command.output_pipe.first<<" "<<c_command.output_pipe.second<<endl;
                    client.user_pipe_map[c_command.user_pipe_output] = create_pipe();
                    string msg = "*** " + client.nickname + " (#" + to_string(client.id) + ") just piped \'" + user_input + "\' to " + clients[c_command.user_pipe_output].nickname + " (#" + to_string(clients[c_command.user_pipe_output].id) + ") ***\n";
                    send_msg(-1, msg, true);
                    c_command.output_pipe = client.user_pipe_map[c_command.user_pipe_output];
                    // cout<<"after command output pipe: "<<c_command.output_pipe.first<<" "<<c_command.output_pipe.second<<endl;
                }
            }

            // if no pipe after this command, execute directly
            execute(c_command, client);
        }

        job_queue.pop();
    }
    return false;
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

                    client_login(clients[i]);
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
                    client_logout(clients[i]);

                } else if (readcount > 0) {

                    // set current client environment variable

                    init_client_env(clients[i]);

                    // process current client command
                    bool terminate_client = process_client_command(clients[i]);
                    if (terminate_client) {
                        client_logout(clients[i]);

                    } else {

                        // unset current client environment variable
                        unset_client_env(clients[i]);

                        // clear buffer
                        memset(clients[i].buffer, '\0', sizeof(clients[i].buffer));
                        send_msg(clients[i].conn_fd, "% ");
                    }
                }
            }
        }
    }

    // close socket
    close(msock);
    FD_CLR(msock, &afds);

    return 0;
}