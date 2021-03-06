/**
 *  @file	tai_shell.cpp
 *  @brief	The shell to execute the TAI APIs
 *  @author	Yuji Hata <yuji.hata@ipinfusion.com>
 *  		Tetsuya Murakami <tetsuya.murakami@ipinfusion.com>
 *  
 *  @copywrite	Copyright (C) 2018 IP Infusion, Inc. All rights reserved.
 *
 *  @remark	This source code is licensed under the Apache license found
 *  		in the LICENSE file in the root directory of this source tree.
 */

#include <thread>
#include <chrono>
#include <iostream>
#include <sstream>
#include <map>
#include <vector>
#include <thread>

#include <mutex>
#include <queue>

#include <sys/eventfd.h>

#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include <dlfcn.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h> 
#include <ext/stdio_filebuf.h> 
#include <poll.h> 
#include <arpa/inet.h>

#include "tai.h"
#include "tai_shell.hpp"
#include "tai_shell.h"

static const char * TAI_CLI_DEFAULT_IP = "0.0.0.0";
static const uint16_t TAI_CLI_DEFAULT_PORT = 4501;

tai_api *p_tai_api;

static int no_of_mods = 0;

pthread_mutex_t tai_shell_mutex = PTHREAD_MUTEX_INITIALIZER;

#if defined(TAISH_API_MODE)
extern "C" {
int tai_shell_cmd_init (int m_max);
int tai_shell_cmd_set_netif_attr (tai_object_id_t m_id, tai_attr_id_t attr_id, tai_attribute_value_t attr_val);
int tai_shell_get_module_id (char *loc_str, tai_object_id_t *m_id);
}
#endif /* defined(TAISH_API_MODE) */

std::map<std::string, tai_object_id_t> location2module_id;

std::map<std::string, tai_command_fn> tai_cli_shell::cmd2handler = {
   {"?", tai_command_help},
   {"help", tai_command_help},
   {"load", tai_command_load},
   {"init", tai_command_init},
   {"quit", tai_command_quit},
   {"exit", tai_command_quit},
   {"logset", tai_command_logset},
   {"set_netif_attr", tai_command_set_netif_attr},
   {"module_list", tai_command_module_list}
};

std::vector <std::string> help_msgs = {
  {"?     : show help messages for all commands\n"},
  {"help  : show help messages for all commands\n"},
  {"load  : load a TAI librarary: Usage: load <TAI librabry file name> \n"},
  {"init  : Initialize TAI API.: Usage: init\n"},
  {"quit  : Quit this session.\n"},
  {"exit  : Exit this session.\n"},
  {"logset: Set log level.: Usage: logset [module|hostif|networkif] [debug|info|notice|warn|error|critical] \n"},
  {"set_netif_attr: Set netif attribute. : Usage: set_netif_attr <module-id> <attr-id> <attr-val> \n"},
  {"module_list: Show the module ID.\n"},
};

tai_module_api_t *module_api;
tai_network_interface_api_t *netif_api;
tai_host_interface_api_t *hostif_api;

int fd;
std::queue<std::pair<bool, std::string>> q;
std::mutex m;

class module {
    public:
        module(tai_object_id_t id) : m_id(id) {
            std::vector<tai_attribute_t> list;
            tai_attribute_t attr;
            attr.id = TAI_MODULE_ATTR_NUM_HOST_INTERFACES;
            list.push_back(attr);
            attr.id = TAI_MODULE_ATTR_NUM_NETWORK_INTERFACES;
            list.push_back(attr);
            auto status = module_api->get_module_attributes(id, list.size(), list.data());
            if ( status != TAI_STATUS_SUCCESS ) {
                throw std::runtime_error("faile to get attribute");
            }
            std::cout << "num hostif: " << list[0].value.u32 << std::endl;
            std::cout << "num netif: " << list[1].value.u32 << std::endl;
            create_hostif(list[0].value.u32);
            create_netif(list[1].value.u32);
        }
        int set_netif_attribute(tai_attr_id_t id, tai_attribute_value_t val);
    private:
        tai_object_id_t m_id;
        std::vector<tai_object_id_t> netifs;
        std::vector<tai_object_id_t> hostifs;
        int create_hostif(uint32_t num);
        int create_netif(uint32_t num);
        int loop();
};

int module::create_netif(uint32_t num) {
    for ( int i = 0; i < num; i++ ) {
        tai_object_id_t id;
        std::vector<tai_attribute_t> list;
        tai_attribute_t attr;

        attr.id = TAI_NETWORK_INTERFACE_ATTR_INDEX;
        attr.value.u32 = i;
        list.push_back(attr);

        auto status = netif_api->create_network_interface(&id, m_id, list.size(), list.data());
        if ( status != TAI_STATUS_SUCCESS ) {
            throw std::runtime_error("failed to create network interface");
        }
        std::cout << "netif: " << id << std::endl;
        netifs.push_back(id);
    }
    return 0;
}

int module::create_hostif(uint32_t num) {
    for ( int i = 0; i < num; i++ ) {
        tai_object_id_t id;
        std::vector<tai_attribute_t> list;
        tai_attribute_t attr;
        attr.id = TAI_HOST_INTERFACE_ATTR_INDEX;
        attr.value.u32 = i;
        list.push_back(attr);
        auto status = hostif_api->create_host_interface(&id, m_id, list.size(), list.data());
        if ( status != TAI_STATUS_SUCCESS ) {
            throw std::runtime_error("failed to create host interface");
        }
        std::cout << "hostif: " << id << std::endl;
        hostifs.push_back(id);
    }
    return 0;
}

int module::set_netif_attribute(tai_attr_id_t attr_id, tai_attribute_value_t attr_val) {
    for (tai_object_id_t id : netifs) {
        std::vector<tai_attribute_t> list;
        tai_attribute_t attr;

        attr.id = attr_id;
        attr.value = attr_val; 
        list.push_back(attr);

        auto status = netif_api->set_network_interface_attributes (id, list.size(), list.data());
        if (status != TAI_STATUS_SUCCESS) {
            throw std::runtime_error("failed to set netif attribute");
        }
    }
    return 0;
}

int module::loop() {
    return 0;
}

std::map<tai_object_id_t, module*> modules;

void module_presence(bool present, char* location) {
    uint64_t v;
    std::lock_guard<std::mutex> g(m);
    q.push(std::pair<bool, std::string>(present, std::string(location)));
    write(fd, &v, sizeof(uint64_t));
}

tai_status_t create_module(const std::string& location, tai_object_id_t& m_id) {
    std::vector<tai_attribute_t> list;
    tai_attribute_t attr;
    attr.id = TAI_MODULE_ATTR_LOCATION;
    attr.value.charlist.count = location.size();
    attr.value.charlist.list = (char*)location.c_str();
    list.push_back(attr);
    return module_api->create_module(&m_id, list.size(), list.data(), nullptr);
}

#if defined(TAISH_API_MODE)
int tai_shell_main(int argc, char *argv[])
#else
int main(int argc, char *argv[])
#endif
{
    tai_cli_server *cli_server;
    struct pollfd fds[3];
    nfds_t nfds;
    std::string ip_str;
    uint16_t port;
    int num_of_fds = 0;
    int timeout = 10 * 1000;
    sockaddr_in addr;
    int c;

    fd = eventfd(0, 0);

    ip_str = std::string(TAI_CLI_DEFAULT_IP);
    port = TAI_CLI_DEFAULT_PORT;

    while ((c = getopt (argc, argv, "i:p:")) != -1) {
      switch (c) {
      case 'i':
        ip_str = std::string(optarg);
        break;

      case 'p':
        port = atoi(optarg);
        break;

      default:
        std::cerr << "Usage: taish -i <IP address> -p <Port number>" << std::endl;
        return 1;
      }
    }

    memset (&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip_str.c_str(), &(addr.sin_addr));

    cli_server = new tai_cli_server(addr);
    if (cli_server == nullptr) {
      return -1;
    }

    fds[0].fd = fd;
    fds[0].events = POLLIN;
    fds[1].fd = cli_server->start();
    fds[1].events = POLLIN;
   
    nfds = 2;

    while (true) {
       int rc = poll(fds, nfds, -1);
       if (rc < 0) {
           if ((errno == EAGAIN) ||
               (errno == EINTR))
               continue;
           return -1;
        }
        if (fds[0].revents == POLLIN) {
            uint64_t v;
            pthread_mutex_lock (&tai_shell_mutex);
            read(fds[0].fd, &v, sizeof(uint64_t));
            {
                std::lock_guard<std::mutex> g(m);
                while ( ! q.empty() ) {
                    auto p = q.front();
                    std::cout << "present: " << p.first << ", loc: " << p.second << std::endl;
                    if ( p.first ) {
                        tai_object_id_t m_id;
                        auto status = create_module(p.second, m_id);
                        if ( status != TAI_STATUS_SUCCESS ) {
                            std::cerr << "failed to create module: " << status << std::endl;
                            return 1;
                        }
                        std::cout << "module id: " << m_id << std::endl;

                        modules[m_id] = new module(m_id);
                        location2module_id.insert(std::pair<std::string, tai_object_id_t>(p.second, m_id));
                    }
                    no_of_mods++;
                    q.pop();
                }
            }
            pthread_mutex_unlock (&tai_shell_mutex);
        }

        if (fds[1].revents == POLLIN) {
            int tmp_fd = cli_server->accept();
            if (tmp_fd > 0) {
                fds[2].fd = tmp_fd;
                nfds = 3;
            } else if (tmp_fd != 0) {
                if (nfds == 3) {
                    cli_server->disconnect();
                    nfds = 2;
                }

                tmp_fd = cli_server->restart();
                if (tmp_fd > 0) {
                    fds[1].fd = tmp_fd;
                } else {
                    std::cerr << "restarting cli server failed " << std::endl;
                    return -1;
                }
            }
        } else if (fds[1].revents != 0) {
            if (nfds == 3) {
                cli_server->disconnect();
                nfds = 2;
            }

            int tmp_fd = cli_server->restart();
            if (tmp_fd > 0) {
                fds[1].fd = tmp_fd;
            } else {
                std::cerr << "restarting cli server failed " << std::endl;
                return -1;
            }
        }

        if ((fds[2].revents == POLLIN) && (nfds == 3)) {
            rc = cli_server->recv();
            if (rc == -10) {
                cli_server->disconnect();
                nfds = 2;
            }
        } else if (fds[2].revents != 0 && (nfds == 3)) {
            cli_server->disconnect();
            nfds = 2;
        }

        for (int i = 0; i < nfds; i++) {
            fds[i].events = POLLIN;
        }
    }
  return 0;
}

tai_cli_server::tai_cli_server(sockaddr_in addr) {
  m_sv_addr = addr;
}

int tai_cli_server::start() {
  int    len, rc, on = 1;
  m_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (m_listen_fd < 0)
  {
    return -1;
  }

  rc = setsockopt(m_listen_fd, SOL_SOCKET,  SO_REUSEADDR,
                  (char *)&on, sizeof(on));
  if (rc < 0)
  {
    close(m_listen_fd);
    m_listen_fd = -1;
    return -1;
  }

  rc = bind(m_listen_fd,
            (sockaddr *)&m_sv_addr, sizeof(m_sv_addr));
  if (rc < 0)
  {
    close(m_listen_fd);
    m_listen_fd = -1;
    return -1;
  }

  rc = listen(m_listen_fd, 0);
  if (rc < 0)
  {
    close(m_listen_fd);
    m_listen_fd = -1;
    return -1;
  }

  return m_listen_fd;
}

int tai_cli_server::restart() {
  close(m_listen_fd);
  m_listen_fd = -1;
  return start();
}

int tai_cli_server::accept() {
  int val;
  int ret;
  socklen_t length;

  if (m_client_fd > 0) {
    return m_client_fd;
  }

  length = sizeof(m_cl_addr);
  m_client_fd = ::accept(m_listen_fd, (sockaddr *)&m_cl_addr, &length);
  if (m_client_fd < 0) {
    if ((errno == EWOULDBLOCK) ||
        (errno == EINTR)) {
      return 0;
    }
    return -1;
  }

  m_ifilebuf = new __gnu_cxx::stdio_filebuf<char>(m_client_fd, std::ios::in);
  m_ofilebuf = new __gnu_cxx::stdio_filebuf<char>(m_client_fd, std::ios::out);
  m_istr = new std::istream(m_ifilebuf);
  m_ostr = new std::ostream(m_ofilebuf);
  return m_client_fd;
}

void tai_cli_server::disconnect() {
  delete m_istr;
  delete m_ostr;
  delete m_ifilebuf;
  delete m_ofilebuf;
  close (m_client_fd);
  m_client_fd = -1;
}

int tai_cli_server::recv() {
  return cmd_parse (m_istr, m_ostr);
}

static void make_args (const std::string& s, std::vector <std::string> *args) {
  char delimiter = 0x20;
  std::string token;
  std::istringstream iss(s);

  while (std::getline(iss, token, delimiter)) {
    args->push_back(token);
  }
}

int tai_cli_shell::cmd_parse(std::istream *istr, std::ostream *ostr) {
  int ret = 0;
  std::string buf;
  std::vector <std::string> *args;
  std::map<std::string, tai_command_fn>::iterator cmd;

  std::getline (*istr, buf);
  if (istr->eof() || istr->fail())
    return -10;

  if (buf.back() == '\r') { // remove CR
    buf.resize(buf.size()-1);
  }

  args = new std::vector <std::string>;
  if (args != nullptr) {
    make_args (buf, args);
    if (args->size() != 0) {
      cmd = cmd2handler.find((*args)[0]);
      if (cmd != cmd2handler.end()) {
        pthread_mutex_lock (&tai_shell_mutex);
        ret = cmd->second(ostr, args);
        pthread_mutex_unlock (&tai_shell_mutex);
      } else {
        *ostr << "unknown command(" << (*args)[0] << ") was speified!!" << std::endl;
      }
    }

    delete args;
  }

  *ostr << "> ";
  ostr->flush();
  return ret;
}

tai_api::tai_api (void *lib_handle) {
  initialize        = (tai_status_t (*)( _In_ uint64_t, _In_ const tai_service_method_table_t *))
                         dlsym(lib_handle, "tai_api_initialize");
  uninitialize      = (tai_status_t (*)(void))
                         dlsym(lib_handle, "tai_api_uninitialize");
  query             = (tai_status_t (*)( _In_ tai_api_t, _Out_ void**))
                         dlsym(lib_handle, "tai_api_query");
  log_set           = (tai_status_t (*)( _In_ tai_api_t, _In_ tai_log_level_t))
                         dlsym(lib_handle, "tai_log_set");
  object_type_query = (tai_object_type_t (*)( _In_ tai_object_id_t))
                         dlsym(lib_handle, "tai_object_type_query");
  module_id_query   = (tai_object_id_t (*)( _In_ tai_object_id_t))
                         dlsym(lib_handle, "tai_module_id_query");
  dbg_generate_dump = (tai_status_t (*)(_In_ const char *))
                         dlsym(lib_handle, "tai_dbg_generate_dump");
  for (auto  i = 0; i < TAI_API_MAX; i++) {
    log_level[i] = TAI_LOG_LEVEL_INFO;
  }
}

int tai_command_load (std::ostream *ostr, std::vector <std::string> *args) {
  void *lib_handle;

  if (args->size() != 2) {
    *ostr << "%% Need to specify the path to TAI library" << std::endl;
    return -1;
  }

  if (p_tai_api != nullptr) {
    *ostr << "%% TAI library is already loaded!!" << std::endl;
    return -1;
  }

  lib_handle = dlopen ((*args)[1].c_str(), RTLD_NOW | RTLD_GLOBAL);

  if (lib_handle == nullptr) {
    *ostr << "%% Loading " << (*args)[1] << " failed!!" << std::endl;
    *ostr << dlerror() << std::endl;
    return -1;
  }
  
  p_tai_api = new tai_api(lib_handle);

  return 0;
}

int tai_command_init (std::ostream *ostr, std::vector <std::string> *args) {
  tai_service_method_table_t services;

  if (args->size() != 1) {
    *ostr << "%% Invalid parameters" << std::endl;
    return -1;
  }

  if (p_tai_api == nullptr) {
    *ostr << "%% Need to load TAI library at first" << std::endl;
    return -1;
  }

  services.module_presence = module_presence;

  if (p_tai_api->log_set) {
    for (auto i = 0; i < TAI_API_MAX; i++) {
      p_tai_api->log_set(tai_api_t(i), p_tai_api->log_level[i]);
    }
  }

  if (p_tai_api->initialize) {
    auto status = p_tai_api->initialize(0, &services);
    if ( status != TAI_STATUS_SUCCESS ) {
      *ostr << "%% Failed to initialize" << std::endl;
      return -1;
    }
  }

  if (p_tai_api->query) {
    auto status = p_tai_api->query(TAI_API_MODULE, (void **)(&module_api));
    if ( status != TAI_STATUS_SUCCESS ) {
      *ostr << "%% Failed to load API for module" << std::endl;
      return -1;
    }

    status = p_tai_api->query(TAI_API_NETWORKIF, (void **)(&netif_api));
    if ( status != TAI_STATUS_SUCCESS ) {
      *ostr << "%% Failed to load API for Network IF" << std::endl;
      return -1;
    }

    status = p_tai_api->query(TAI_API_HOSTIF, (void **)(&hostif_api));
    if ( status != TAI_STATUS_SUCCESS ) {
      *ostr << "%% Failed to load API for Host IF" << std::endl;
      return -1;
    }
  }

  return 0;
}

int tai_command_help (std::ostream *ostr, std::vector <std::string> *args) {
  for (auto &m : help_msgs)
    *ostr << m;

  return 0;
}

int tai_command_quit (std::ostream *ostr, std::vector <std::string> *args) {
  return -10;
}

int tai_command_logset (std::ostream *ostr, std::vector <std::string> *args) {
  tai_api_t id;
  if (args->size() != 3) {
    *ostr << "%% Invalid parameters" << std::endl;
    return -1;
  }

  if (p_tai_api == nullptr) {
    *ostr << "%% Need to load TAI library at first" << std::endl;
    return -1;
  }

  auto val = (*args)[1];
  if (val == "unspecified") {
    id = TAI_API_UNSPECIFIED;
  } else if (val == "module") {
    id = TAI_API_MODULE;
  } else if (val == "hostif") {
    id = TAI_API_HOSTIF;
  } else if (val == "networkif") {
    id = TAI_API_NETWORKIF;
  } else {
    *ostr << "%% Invalid argument (unspecified, hostif or networkif)" << std::endl;
    return -1;
  }

  auto level = (*args)[2];
  if (level == "debug") {
    p_tai_api->log_level[id] = TAI_LOG_LEVEL_DEBUG;

  } else if (level == "info") {
    p_tai_api->log_level[id] = TAI_LOG_LEVEL_INFO;

  } else if (level == "notice") {
    p_tai_api->log_level[id] = TAI_LOG_LEVEL_NOTICE;

  } else if (level == "warn") {
    p_tai_api->log_level[id] = TAI_LOG_LEVEL_WARN;

  } else if (level == "error") {
    p_tai_api->log_level[id] = TAI_LOG_LEVEL_ERROR;

  } else if (level == "critical") {
    p_tai_api->log_level[id] = TAI_LOG_LEVEL_CRITICAL;

  } else {
    *ostr << "Invalid log-level(" << (*args)[1] << ") was specified!!" << std::endl;
    return -1;
  }

  if (p_tai_api->log_set) {
    p_tai_api->log_set(id, p_tai_api->log_level[id]);
  }

  return 0;
}

int tai_command_set_netif_attr (std::ostream *ostr, std::vector <std::string> *args) {
  tai_object_id_t id;  
  tai_attr_id_t attr;
  tai_attribute_value_t attr_val;

  if (args->size() == 1) {
    *ostr << "Usage: set_netif_attr <module-id> <attr-id> <attr-val>" << std::endl;
    *ostr << "    <module-id>: integer." << std::endl;
    *ostr << "    <attr-id> : tx-enable, tx-grid, tx-channel, output-power, tx-laser-freq, modulation or differential-encoding." << std::endl;
    *ostr << "    <attr-val> :  tx-enable: true or false" << std::endl;
    *ostr << "                  tx-grid: 100, 50, 33, 25, 12.5 or 6.25" << std::endl;
    *ostr << "                  tx-channel: integer" << std::endl;
    *ostr << "                  output-power: float" << std::endl;
    *ostr << "                  tx-laser-freq: integer" << std::endl;
    *ostr << "                  modulation: bpsk, dp-bpsk, qpsk, dp-qpsk, 8qam, dp-8qam, 16qam, dp-16qam, 32qam, dp-32qam, 64qam or dp-64qam" << std::endl;
    *ostr << "                  differential-encoding: true or false" << std::endl;
    return -1;
  }

  if (args->size() != 4) {
    *ostr << "%% Invalid parameters" << std::endl;
    return -1;
  }

  if (p_tai_api == nullptr) {
    *ostr << "%% Need to load TAI library at first" << std::endl;
    return -1;
  }

  id = std::stoull((*args)[1], nullptr, 10);
  if (modules[id] == nullptr) {
    *ostr << "%% Invalid module ID" << std::endl;
    return -1;
  }

  auto com = (*args)[2];
  if (com == "tx-enable") {
    attr = TAI_NETWORK_INTERFACE_ATTR_TX_ENABLE;
  } else if (com == "tx-grid") {
    attr = TAI_NETWORK_INTERFACE_ATTR_TX_GRID_SPACING;
  } else if (com == "tx-channel") {
    attr = TAI_NETWORK_INTERFACE_ATTR_TX_CHANNEL;
  } else if (com == "output-power") {
    attr = TAI_NETWORK_INTERFACE_ATTR_OUTPUT_POWER;
  } else if (com == "tx-laser-freq") {
    attr = TAI_NETWORK_INTERFACE_ATTR_TX_FINE_TUNE_LASER_FREQ;
  } else if (com == "modulation") {
    attr = TAI_NETWORK_INTERFACE_ATTR_MODULATION_FORMAT;
  } else if (com == "differential-encoding") { 
    attr = TAI_NETWORK_INTERFACE_ATTR_DIFFERENTIAL_ENCODING;
  } else {
    *ostr << "Invalid attribute (tx-enable, tx-grid, tx-channel, output-power, tx-laser-freq, modulation or differential-encoding)" << std::endl;
    return -1;
  }

  if (attr == TAI_NETWORK_INTERFACE_ATTR_TX_ENABLE) {
    auto val = (*args)[3];
    if (val == "true") {
      attr_val.booldata = true;
    } else if (val == "false") {
      attr_val.booldata = false;
    } else {
      *ostr << "%% Invalid argument (true or false)" << std::endl;
      return -1;
    }
  } else if (attr == TAI_NETWORK_INTERFACE_ATTR_TX_GRID_SPACING) {
    auto val = (*args)[3];
    if (val == "100") {
      attr_val.u32 = TAI_NETWORK_INTERFACE_TX_GRID_SPACING_100_GHZ;
    } else if (val == "50") {
      attr_val.u32 = TAI_NETWORK_INTERFACE_TX_GRID_SPACING_50_GHZ;
    } else if (val == "33") {
      attr_val.u32 = TAI_NETWORK_INTERFACE_TX_GRID_SPACING_33_GHZ;
    } else if (val == "25") {
      attr_val.u32 = TAI_NETWORK_INTERFACE_TX_GRID_SPACING_25_GHZ;
    } else if (val == "12.5") {
      attr_val.u32 = TAI_NETWORK_INTERFACE_TX_GRID_SPACING_12_5_GHZ;
    } else if (val == "6.25") {
      attr_val.u32 = TAI_NETWORK_INTERFACE_TX_GRID_SPACING_6_25_GHZ;
    } else {
      *ostr << "%% Invalid argument (100, 50, 33, 25, 12.5 or 6.25" << std::endl;
      return -1;
    }
  } else if (attr == TAI_NETWORK_INTERFACE_ATTR_TX_CHANNEL) {
    auto val = std::stoi((*args)[3], nullptr, 10);
    attr_val.u16 = val;
  } else if (attr == TAI_NETWORK_INTERFACE_ATTR_OUTPUT_POWER) {
    auto val = std::stof((*args)[3], nullptr);
    attr_val.flt = val;
  } else if (attr == TAI_NETWORK_INTERFACE_ATTR_TX_FINE_TUNE_LASER_FREQ) {
    auto val = std::stoull((*args)[3], nullptr, 10);
    attr_val.u64 = val;
  } else if (attr == TAI_NETWORK_INTERFACE_ATTR_MODULATION_FORMAT) {
    auto val = (*args)[3];
    if (val == "bpsk") {
      attr_val.u32 = TAI_NETWORK_INTERFACE_MODULATION_FORMAT_BPSK;
    } else if (val == "dp-bpsk") {
      attr_val.u32 = TAI_NETWORK_INTERFACE_MODULATION_FORMAT_DP_BPSK;
    } else if (val == "qpsk") {
      attr_val.u32 = TAI_NETWORK_INTERFACE_MODULATION_FORMAT_QPSK;
    } else if (val == "dp-qpsk") {
      attr_val.u32 = TAI_NETWORK_INTERFACE_MODULATION_FORMAT_DP_QPSK;
    } else if (val == "8qam") {
      attr_val.u32 = TAI_NETWORK_INTERFACE_MODULATION_FORMAT_8_QAM;
    } else if (val == "dp-8qam") {
      attr_val.u32 = TAI_NETWORK_INTERFACE_MODULATION_FORMAT_DP_8_QAM;
    } else if (val == "16qam") {
      attr_val.u32 = TAI_NETWORK_INTERFACE_MODULATION_FORMAT_16_QAM;
    } else if (val == "dp-16qam") {
      attr_val.u32 = TAI_NETWORK_INTERFACE_MODULATION_FORMAT_DP_16_QAM;
    } else if (val == "32qam") {
      attr_val.u32 = TAI_NETWORK_INTERFACE_MODULATION_FORMAT_32_QAM;
    } else if (val == "dp-32qam") {
      attr_val.u32 = TAI_NETWORK_INTERFACE_MODULATION_FORMAT_DP_32_QAM;
    } else if (val == "64qam") {
      attr_val.u32 = TAI_NETWORK_INTERFACE_MODULATION_FORMAT_64_QAM;
    } else if (val == "dp-64qam") {
      attr_val.u32 = TAI_NETWORK_INTERFACE_MODULATION_FORMAT_DP_64_QAM;
    } else {
      *ostr << "%% Invalid argument (bpsk, dp-bpsk, qpsk, dp-qpsk, 8qam, dp-8qam, 16qam, dp-16qam, 32qam, dp-32qam, 64qam or dp-64qam)" << std::endl;
      return -1;
    }
  } else if (attr == TAI_NETWORK_INTERFACE_ATTR_DIFFERENTIAL_ENCODING) {
    auto val = (*args)[3];
    if (val == "true") {
      attr_val.booldata = true;
    } else if (val == "false") {
      attr_val.booldata = false;
    } else {
      *ostr << "%% Invalid argument (true or false)" << std::endl;
      return -1;
    }
  } else {
    *ostr << "%% Invalid Attribute ID" << std::endl;
    return -1;
  }

  modules[id]->set_netif_attribute (attr, attr_val);
  return 0;
}

int tai_command_module_list (std::ostream *ostr, std::vector <std::string> *args) {
  if (args->size() != 1) {
    *ostr << "Usage: module_list" << std::endl;
    return -1;
  }

  *ostr << "Module List" << std::endl;
  for (auto loc2mod : location2module_id)
    *ostr << "loacation: " << loc2mod.first << "  module ID: " << loc2mod.second << std::endl;
  return 0;
}

#if defined(TAISH_API_MODE)

int tai_shell_thread (std::vector <std::string> args) {
  int argc = 0;
  char *argv[5];

  for (argc = 0; argc < args.size(); argc++) {
    argv[argc] = (char *)args[argc].c_str();
  }

  return tai_shell_main (argc, argv);
}

int tai_shell_start (uint16_t port, char *ip_addr)
{
  std::vector <std::string> args;

  args.push_back("taish");

  if (ip_addr != nullptr) {
    args.push_back("-i");
    args.push_back(ip_addr);
  } else {
    args.push_back("-i");
    args.push_back(TAI_CLI_DEFAULT_IP);
  }

  if (port != 0) {
    args.push_back("-p");
    args.push_back(std::to_string(port));
  } else {
    args.push_back("-p");
    args.push_back(std::to_string(TAI_CLI_DEFAULT_PORT));
  }

  std::thread th(&tai_shell_thread, args);
  th.detach();

  return 0;
}

int tai_shell_cmd_load (char *library_fiLe_name, tai_sh_api_t *tai_api)
{
  int ret;
  std::vector <std::string> args;

  args.push_back("load");
  args.push_back(library_fiLe_name);

  pthread_mutex_lock (&tai_shell_mutex);
  ret = tai_command_load (&std::cout, &args);
  pthread_mutex_unlock (&tai_shell_mutex);

  if (ret < 0) {
    return ret;
  }

  /* TAI API */
  tai_api->initialize =        p_tai_api->initialize;
  tai_api->query =             p_tai_api->query;
  tai_api->uninitialize =      p_tai_api->uninitialize;
  tai_api->log_set =           p_tai_api->log_set;
  tai_api->object_type_query = p_tai_api->object_type_query;
  tai_api->module_id_query =   p_tai_api->module_id_query;
  tai_api->dbg_generate_dump = p_tai_api->dbg_generate_dump;
  tai_api->lock =              &tai_shell_mutex;

  /* TAI Shell Specific APIs */
  tai_api->taish_init =           tai_shell_cmd_init;
  tai_api->taish_set_netif_attr = tai_shell_cmd_set_netif_attr;
  tai_api->taish_get_module_id =  tai_shell_get_module_id;

  return 0;
}

/*
 * TAI Shell Spcific APIs
 */

int tai_shell_cmd_init (int m_max)
{
  int ret;
  std::vector <std::string> args;

  args.push_back("init");
  pthread_mutex_lock (&tai_shell_mutex);
  ret = tai_command_init (&std::cout, &args);
  pthread_mutex_unlock (&tai_shell_mutex);

  while (no_of_mods <  m_max) {
    usleep (1000);
  }

  return ret;
}

int tai_shell_cmd_set_netif_attr (tai_object_id_t m_id, tai_attr_id_t attr_id, tai_attribute_value_t attr_val)
{
  if (p_tai_api == nullptr) {
    std::cout << "%% Need to load TAI library at first" << std::endl;
    return -1;
  }

  if (modules[m_id] == nullptr) {
    std::cout << "%% Invalid module ID" << std::endl;
    return -1;
  }

  pthread_mutex_lock (&tai_shell_mutex);
  modules[m_id]->set_netif_attribute (attr_id, attr_val);
  pthread_mutex_unlock (&tai_shell_mutex);

  return 0;
}

int tai_shell_get_module_id (char *loc_str, tai_object_id_t *m_id)
{
  std::string loc(loc_str);
  std::map<std::string, tai_object_id_t>::iterator loc2modResult;

  loc2modResult = location2module_id.find(loc);
  if (loc2modResult != location2module_id.end()) {
    *m_id = loc2modResult->second;
    return 0;
  }
  return -1;
}

#endif /* defined(TAISH_API_MODE) */

