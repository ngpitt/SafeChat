/*
  Copyright Xphysics 2012. All Rights Reserved.

  SafeChat is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version.

  SafeChat is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE. See the GNU General Public License for more details.

  <http://www.gnu.org/licenses/>
 */

#include "client.h"

Client::Client(int argc, char **argv) {

    std::string string;
    std::ifstream config_file;

    _network_data = _terminal_data = _encryption = false;
    time(&_time);
    pthread_cond_init(&_cond, NULL);
    pthread_mutex_init(&_mutex, NULL);
    _config_path = std::string(getenv("HOME")) + "/.safechat";
    try {
        config_file.open(_config_path.c_str());
        if (!config_file)
            throw std::runtime_error("can't read config file");
        while (std::getline(config_file, string))
            if (string.substr(0, 11) == "local_name=")
                _name = string.substr(11);
            else if (string.substr(0, 7) == "server=")
                _server = string.substr(7);
            else if (string.substr(0, 5) == "port=")
                _port = atoi(string.substr(5).c_str());
            else if (string.substr(0, 10) == "file_path=")
                _file_path = string.substr(10);
        config_file.close();
    } catch (const std::exception &exception) {
        std::cerr << "Error: " << exception.what() << ".\n";
    }
    try {
        for (int i = 1; i < argc; i++) {
            string = argv[i];
            if (string == "-n" && i + 1 < argc)
                _name = argv[++i];
            else if (string == "-s" && i + 1 < argc)
                _server = argv[++i];
            else if (string == "-p" && i + 1 < argc)
                _port = atoi(argv[++i]);
            else if (string == "-f" && i + 1 < argc)
                _file_path = argv[++i];
            else
                throw std::runtime_error("unknown argument " + std::string(argv[i]));
        }
        if (_name.size() < 1)
            throw std::runtime_error("name required");
        if (_server.size() < 1)
            throw std::runtime_error("server required");
        if (_port < 1 || _port > 65535)
            throw std::runtime_error("invalid port number");
        if (_file_path.size() < 1)
            throw std::runtime_error("file transfer path required");
    } catch (const std::exception &exception) {
        std::cout << "SafeChat (version " << std::fixed << std::setprecision(1) << __version << ") - (c) 2013 Nicholas Pitt\nhttps://www.xphysics.net/\n\n    -n <name> Specifies the name forwarded to the SafeChat server (use quotes)\n    -s <serv> Specifies the DNS name or IP address of a SafeChat server\n    -p <port> Specifies the port the SafeChat server is running on\n    -f <path> Specifies the file transfer path (use quotes)\n" << std::endl;
        std::cerr << "Error: " << exception.what() << ".\n";
        exit(EXIT_FAILURE);
    }
    _file_path = trim_path(_file_path);
    if (_file_path[_file_path.size() - 1] != '/')
        _file_path += "/";
}

Client::~Client() {

    std::ofstream config_file;

    pthread_kill(_terminal_listener, SIGTERM);
    pthread_kill(_keepalive_sender, SIGTERM);
    pthread_kill(_network_listener, SIGTERM);
    close(_socket);
    pthread_mutex_unlock(&_mutex);
    pthread_cond_destroy(&_cond);
    pthread_mutex_destroy(&_mutex);
    if (_encryption) {
        EVP_CIPHER_CTX_cleanup(&_encryption_ctx);
        EVP_CIPHER_CTX_cleanup(&_decryption_ctx);
    }
    try {
        config_file.open(_config_path.c_str());
        if (!config_file)
            throw std::runtime_error("can't write config file");
        config_file << "Config file for SafeChat\n\nlocal_name=" << _name << "\nserver=" << _server << "\nport=" << _port << "\nfile_path=" << _file_path;
        config_file.close();
    } catch (const std::exception &exception) {
        std::cerr << "Error: " << exception.what() << ".";
    }
    std::cout << std::endl;
}

int Client::start() {

    int id, hosts_size, choice;
    std::string string;
    sockaddr_in addr;
    hostent *host;
    DH *dh = DH_new();
    BIGNUM *pub_key = BN_new();
    hosts_t hosts;
    block_t block;
    block_t::cmd_t response;

    try {
        _socket = socket(AF_INET, SOCK_STREAM, 0);
        host = gethostbyname(_server.c_str());
        if (!host)
            throw std::runtime_error("can't resolve server name");
        addr.sin_family = AF_INET;
        memcpy(&addr.sin_addr.s_addr, host->h_addr_list[0], host->h_length);
        addr.sin_port = htons(_port);
        if (connect(_socket, (sockaddr *) & addr, sizeof addr))
            throw std::runtime_error("can't connect to server");
        pthread_create(&_terminal_listener, NULL, &Client::terminal_listener, this);
        pthread_create(&_keepalive_sender, NULL, &Client::keepalive_sender, this);
        pthread_create(&_network_listener, NULL, &Client::network_listener, this);
        recv_block(block);
        if (*(int *) block._data != (int) __version)
            throw std::runtime_error("incompatible server version");
        recv_block(block);
        if (*(bool *) block._data)
            throw std::runtime_error("server is full");
        send_block(block_t(block_t::name, _name.c_str(), _name.size() + 1));
        while (true) {
            do {
                std::cout << "\nMain Menu\n\n    1) Start new host\n    2) Connect to host\n\nChoice: " << std::flush;
                get_string(string);
            } while (string != "1" && string != "2");
            if (string == "1") {
                send_block(block_t(block_t::host));
                do {
                    std::cout << "\nWaiting for client to connect..." << std::flush;
                    recv_block(block);
                    _peer_name = (char *) block._data;
                    std::cout << std::endl;
                    do {
                        std::cout << "Accept connection from " << _peer_name << "? (y/n) " << std::flush;
                        get_string(string);
                    } while (string != "y" && string != "n");
                    if (string == "y") {
                        send_block(block_t(block_t::accept));
                        DH_generate_parameters_ex(dh, __key_length, 5, NULL);
                        block = block_t(block_t::data, BN_num_bytes(dh->p));
                        BN_bn2bin(dh->p, block._data);
                        send_block(block);
                        DH_generate_key(dh);
                        block = block_t(block_t::data, BN_num_bytes(dh->pub_key));
                        BN_bn2bin(dh->pub_key, block._data);
                        send_block(block);
                        recv_block(block);
                        BN_bin2bn(block._data, block._size, pub_key);
                        DH_compute_key(_key, pub_key, dh);
                        BN_free(pub_key);
                        DH_free(dh);
                        shell();
                    } else if (string == "n")
                        send_block(block_t(block_t::decline));
                } while (string == "n");
            } else if (string == "2") {
                do {
                    send_block(block_t(block_t::list));
                    recv_block(block);
                    hosts_size = *(int *) block._data;
                    if (!hosts_size) {
                        std::cout << "\nNo available hosts." << std::endl;
                        break;
                    }
                    hosts.clear();
                    for (int i = 0; i < hosts_size; i++) {
                        recv_block(block);
                        id = *(int *) block._data;
                        recv_block(block);
                        hosts.push_back(std::make_pair(id, (char *) block._data));
                    }
                    do {
                        std::cout << "\nHosts:\n" << std::endl;
                        for (int i = 0; i < hosts_size; i++)
                            std::cout << "    " << i + 1 << ") " << hosts[i].second << std::endl;
                        std::cout << "\nChoice: " << std::flush;
                        get_string(string);
                        choice = atoi(string.c_str()) - 1;
                    } while (choice < 0 || choice >= hosts_size);
                    _peer_name = hosts[choice].second;
                    send_block(block_t(block_t::request, &hosts[choice].first, sizeof hosts[choice].first));
                    std::cout << "\nWaiting for " << _peer_name << " to accept your connection..." << std::flush;
                    recv_block(block);
                    response = block._cmd;
                    if (response == block_t::accept) {
                        std::cout << std::endl;
                        DH_generate_parameters_ex(dh, __key_length, 5, NULL);
                        recv_block(block);
                        BN_bin2bn(block._data, block._size, dh->p);
                        DH_generate_key(dh);
                        block = block_t(block_t::data, BN_num_bytes(dh->pub_key));
                        BN_bn2bin(dh->pub_key, block._data);
                        send_block(block);
                        recv_block(block);
                        BN_bin2bn(block._data, block._size, pub_key);
                        DH_compute_key(_key, pub_key, dh);
                        BN_free(pub_key);
                        DH_free(dh);
                        shell();
                    } else if (response == block_t::decline)
                        std::cout << "\n" << _peer_name << " declined your connection." << std::endl;
                    else if (response == block_t::unavailable)
                        std::cout << "\n" << _peer_name << " is unavailable." << std::endl;
                } while (response != block_t::accept);
            }
        }
    } catch (const std::exception &exception) {
        std::cerr << "Error: " << exception.what() << ".";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

void Client::shell() {

    enum response_t {
        accept, decline
    } response;
    unsigned int data_size = __block_size - AES_BLOCK_SIZE;
    long file_size, bytes_sent, bytes_remaining, rate, time_elapsed;
    unsigned char key[32], iv[32];
    std::string string, file_path, file_name;
    std::ofstream out_file;
    std::ifstream in_file;
    time_t start_time;
    block_t block;

    EVP_BytesToKey(EVP_aes_256_cbc(), EVP_sha256(), (unsigned char *) "SafeChat", _key, __key_length, 5, key, iv);
    EVP_CIPHER_CTX_init(&_encryption_ctx);
    EVP_CIPHER_CTX_init(&_decryption_ctx);
    EVP_EncryptInit_ex(&_encryption_ctx, EVP_aes_256_cbc(), NULL, key, iv);
    EVP_DecryptInit_ex(&_decryption_ctx, EVP_aes_256_cbc(), NULL, key, iv);
    _encryption = true;
    std::cout << "\nCommands:\n\n    <path> - Transfer file\n    <entr> - Disconnect\n" << std::endl;
    while (true) {
        try {
            pthread_mutex_lock(&_mutex);
            std::cout << _name << ": " << std::flush;
            while (!_network_data && !_terminal_data)
                pthread_cond_wait(&_cond, &_mutex);
            if (_network_data) {
                if (_block._data[0] == '/') {
                    file_name = (char *) _block._data;
                    file_name = file_name.substr(1);
                    _network_data = false;
                    pthread_cond_signal(&_cond);
                    pthread_mutex_unlock(&_mutex);
                    recv_block(block);
                    file_size = *(long *) block._data;
                    std::cout << std::endl;
                    do {
                        std::cout << "Accept transfer of " << file_name << " (" << format_size(file_size) << ")? (y/n) " << std::flush;
                        get_string(string);
                    } while (string != "y" && string != "n");
                    if (string == "y") {
                        file_path = _file_path + file_name;
                        out_file.open(file_path.c_str(), std::ofstream::binary);
                        if (!out_file) {
                            response = decline;
                            send_block(block_t(block_t::data, &response, sizeof response));
                            throw std::runtime_error("can't write file");
                        }
                        response = accept;
                        send_block(block_t(block_t::data, &response, sizeof response));
                        bytes_sent = 0;
                        bytes_remaining = file_size;
                        time(&start_time);
                        std::cout << "\r" << std::string(80, ' ') << "\rReceiving " << file_name << "..." << std::flush;
                        do {
                            recv_block(block);
                            out_file.write((char *) block._data, block._size);
                            bytes_sent += block._size;
                            bytes_remaining -= block._size;
                            time_elapsed = difftime(time(NULL), start_time);
                            std::cout << "\r" << std::string(80, ' ') << "\rReceiving " << file_name << "... " << std::fixed << std::setprecision(0) << (double) bytes_sent / file_size * 100 << "%" << std::flush;
                            if (time_elapsed) {
                                rate = bytes_sent / time_elapsed;
                                std::cout << " (" << format_time(bytes_remaining / rate) << " at " << format_size(rate) << "/s)" << std::flush;
                            }
                        } while (bytes_sent < file_size);
                        std::cout << std::endl;
                        out_file.close();
                    } else if (string == "n") {
                        response = decline;
                        send_block(block_t(block_t::data, &response, sizeof response));
                    }
                } else {
                    std::cout << "\r" << _peer_name << ": " << _block._data << std::endl;
                    _network_data = false;
                    pthread_cond_signal(&_cond);
                    pthread_mutex_unlock(&_mutex);
                }
            } else {
                file_path = trim_path(_string);
                if (file_path[0] == '/') {
                    _terminal_data = false;
                    pthread_cond_signal(&_cond);
                    pthread_mutex_unlock(&_mutex);
                    in_file.open(file_path.c_str(), std::ifstream::binary);
                    if (!in_file)
                        throw std::runtime_error("can't read file");
                    file_name = file_path.substr(file_path.rfind("/"));
                    send_block(block_t(block_t::data, file_name.c_str(), file_name.size() + 1));
                    file_name = file_name.substr(1);
                    in_file.seekg(0, std::ifstream::end);
                    file_size = in_file.tellg();
                    in_file.seekg(0, std::ifstream::beg);
                    send_block(block_t(block_t::data, &file_size, sizeof file_size));
                    std::cout << "Waiting for " << _peer_name << " to accept the file transfer..." << std::flush;
                    recv_block(block);
                    response = *(response_t *) block._data;
                    if (response == accept) {
                        bytes_sent = 0;
                        bytes_remaining = file_size;
                        time(&start_time);
                        std::cout << "\nSending " << file_name << "..." << std::flush;
                        do {
                            if (bytes_remaining > data_size)
                                block._size = data_size;
                            else
                                block._size = bytes_remaining;
                            block = block_t(block_t::data, block._size);
                            in_file.read((char *) block._data, block._size);
                            send_block(block);
                            bytes_sent += block._size;
                            bytes_remaining -= block._size;
                            time_elapsed = difftime(time(NULL), start_time);
                            std::cout << "\r" << std::string(80, ' ') << "\rSending " << file_name << "... " << std::fixed << std::setprecision(0) << (double) bytes_sent / file_size * 100 << "%" << std::flush;
                            if (time_elapsed) {
                                rate = bytes_sent / time_elapsed;
                                std::cout << " (" << format_time(bytes_remaining / rate) << " at " << format_size(rate) << "/s)" << std::flush;
                            }
                        } while (bytes_remaining);
                        std::cout << "\n" << std::flush;
                    } else if (response == decline)
                        std::cout << "\n" << _peer_name << " declined the file transfer." << std::endl;
                    in_file.close();
                } else {
                    if (_string.size() + 1 > data_size)
                        block._size = data_size;
                    else
                        block._size = _string.size() + 1;
                    send_block(block_t(block_t::data, _string.c_str(), block._size));
                    _terminal_data = false;
                    pthread_cond_signal(&_cond);
                    pthread_mutex_unlock(&_mutex);
                }
            }
        } catch (const std::exception &exception) {
            std::cerr << "Error: " << exception.what() << ".\n";
        }
    }
}

void *Client::keepalive_sender() {

    int interval = __timeout / 3;

    signal(SIGTERM, thread_handler);
    while (true) {
        sleep(interval);
        if (difftime(time(NULL), _time) > interval)
            send_block(block_t(block_t::keepalive));
    }
    return NULL;
}

void *Client::network_listener() {

    int padding;
    block_t recv_block;

    signal(SIGTERM, thread_handler);
    try {
        while (true) {
            if (!recv(_socket, &recv_block._cmd, sizeof recv_block._cmd, MSG_WAITALL))
                throw std::runtime_error("connection dropped");
            if (!recv(_socket, &recv_block._size, sizeof recv_block._size, MSG_WAITALL))
                throw std::runtime_error("connection dropped");
            if (recv_block._size > __block_size)
                throw std::runtime_error("oversized block received");
            if (recv_block._size)
                if (!recv(_socket, recv_block._data, recv_block._size, MSG_WAITALL))
                    throw std::runtime_error("connection dropped");
            if (recv_block._cmd == block_t::disconnect) {
                std::cout << "\n\nDisconnected.\n" << std::endl;
                exit(EXIT_SUCCESS);
            }
            if (_encryption && recv_block._size) {
                _block = block_t(recv_block._cmd);
                EVP_DecryptInit_ex(&_decryption_ctx, NULL, NULL, NULL, NULL);
                EVP_DecryptUpdate(&_decryption_ctx, _block._data, &_block._size, recv_block._data, recv_block._size);
                EVP_DecryptFinal_ex(&_decryption_ctx, _block._data + _block._size, &padding);
                _block._size += padding;
            } else
                _block = recv_block;
            _network_data = true;
            pthread_mutex_lock(&_mutex);
            pthread_cond_signal(&_cond);
            while (_network_data)
                pthread_cond_wait(&_cond, &_mutex);
            pthread_mutex_unlock(&_mutex);
        }
    } catch (const std::exception &exception) {
        std::cerr << "\nError: " << exception.what() << ".\n";
        exit(EXIT_FAILURE);
    }
    return NULL;
}

void *Client::terminal_listener() {
    signal(SIGTERM, thread_handler);
    while (true) {
        std::getline(std::cin, _string);
        if (!_string.size()) {
            send_block(block_t(block_t::disconnect));
            std::cout << "\nDisconnected.\n" << std::endl;
            exit(EXIT_SUCCESS);
        }
        _terminal_data = true;
        pthread_mutex_lock(&_mutex);
        pthread_cond_signal(&_cond);
        while (_terminal_data)
            pthread_cond_wait(&_cond, &_mutex);
        pthread_mutex_unlock(&_mutex);
    }
    return NULL;
}

void Client::send_block(const block_t &block) {

    int padding;
    block_t send_block;

    signal(SIGPIPE, SIG_IGN);
    if (_encryption && block._size) {
        send_block = block_t(block._cmd);
        EVP_EncryptInit_ex(&_encryption_ctx, NULL, NULL, NULL, NULL);
        EVP_EncryptUpdate(&_encryption_ctx, send_block._data, &send_block._size, block._data, block._size);
        EVP_EncryptFinal_ex(&_encryption_ctx, send_block._data + send_block._size, &padding);
        send_block._size += padding;
    } else
        send_block = block;
    send(_socket, &send_block._cmd, sizeof send_block._cmd, 0);
    send(_socket, &send_block._size, sizeof send_block._size, 0);
    if (send_block._size)
        send(_socket, send_block._data, send_block._size, 0);
    time(&_time);
}

void Client::recv_block(block_t &block) {
    pthread_mutex_lock(&_mutex);
    while (!_network_data)
        pthread_cond_wait(&_cond, &_mutex);
    block = _block;
    _network_data = false;
    pthread_cond_signal(&_cond);
    pthread_mutex_unlock(&_mutex);
}

void Client::get_string(std::string &string) {
    pthread_mutex_lock(&_mutex);
    while (!_terminal_data)
        pthread_cond_wait(&_cond, &_mutex);
    string = _string;
    _terminal_data = false;
    pthread_cond_signal(&_cond);
    pthread_mutex_unlock(&_mutex);
}

std::string Client::trim_path(std::string path) {

    size_t pos;

    if (path.size()) {
        path = path.substr(0, path.find_last_not_of(" '\"") + 1);
        path = path.substr(path.find_first_not_of(" '\""));
        pos = path.find("\\");
        while (pos != path.npos) {
            path.erase(pos, 1);
            pos = path.find("\\");
        }
    }
    return path;
}

std::string Client::format_size(long bytes) {

    double gb = 1024 * 1024 * 1024, mb = 1024 * 1024, kb = 1024;
    std::string string;
    std::stringstream stream;

    gb = bytes / gb;
    mb = bytes / mb;
    kb = bytes / kb;
    if (gb >= 1) {
        stream << std::fixed << std::setprecision(1) << gb;
        string = stream.str() + " GB";
    } else if (mb >= 1) {
        stream << std::fixed << std::setprecision(1) << mb;
        string = stream.str() + " MB";
    } else if (kb >= 1) {
        stream << std::fixed << std::setprecision(0) << kb;
        string = stream.str() + " KB";
    } else {
        stream << bytes;
        string = stream.str() + " B";
    }
    return string;
}

std::string Client::format_time(long seconds) {

    int hrs = 60 * 60, min = 60;
    std::string string;
    std::stringstream stream;

    if (seconds / hrs >= 1) {
        stream << seconds / hrs;
        string = stream.str() + " hrs";
    } else if (seconds / min >= 1) {
        stream << seconds / min;
        string = stream.str() + " min";
    } else {
        stream << seconds;
        string = stream.str() + " sec";
    }
    return string;
}
