#include "loxone_component.h"

#include <cerrno>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"

namespace esphome {
  namespace loxone {
    static const uint32_t TCP_CONNECT_RETRY_INTERVAL_MS = 5000;
    static const uint8_t MAX_PACKETS_PER_LOOP = 8;

    void LoxoneComponent::setup() {

    }

    void LoxoneComponent::dump_config() {
      ESP_LOGCONFIG(TAG, "Loxone:");
      ESP_LOGCONFIG(TAG, "  protocol: %s", this->protocol_.c_str());
      ESP_LOGCONFIG(TAG, "  loxone address: %s:%u", this->loxone_ip_.c_str(), this->loxone_port_);
      ESP_LOGCONFIG(TAG, "  listen port: %u", this->listen_port_);
    }

    void LoxoneComponent::teardown_() {
      this->udp_socket_ = nullptr;
      this->tcp_listen_socket_ = nullptr;
      this->tcp_client_socket_ = nullptr;
      this->tcp_server_clients_.clear();
      this->server_ready_ = false;
      this->client_ready_ = false;
      this->tcp_connecting_ = false;
    }

    void LoxoneComponent::ensure_listen_udp() {
      if (this->protocol_ != "udp") {
        return;
      }

      if (this->server_ready_) {
        return;
      }

      this->udp_socket_ = socket::socket_ip(SOCK_DGRAM, IPPROTO_UDP);
      if (this->udp_socket_ == nullptr) {
        ESP_LOGW(TAG, "could not create udp socket");
        return;
      }
      this->udp_socket_->setblocking(false);

      struct sockaddr_storage bind_addr;
      socklen_t addr_len = socket::set_sockaddr_any((struct sockaddr *) &bind_addr, sizeof(bind_addr), this->listen_port_);
      if (this->udp_socket_->bind((struct sockaddr *) &bind_addr, addr_len) != 0) {
        ESP_LOGW(TAG, "udp bind to port %u failed: errno %d", this->listen_port_, errno);
        this->udp_socket_ = nullptr;
        return;
      }

      this->server_ready_ = true;
      // UDP is connectionless: once the socket is up, we can send
      this->client_ready_ = true;
      ESP_LOGD(TAG, "listening on udp port %u", this->listen_port_);
    }

    void LoxoneComponent::poll_udp() {
      if (this->protocol_ != "udp" || this->udp_socket_ == nullptr) {
        return;
      }

      uint8_t buf[1024];
      for (uint8_t i = 0; i < MAX_PACKETS_PER_LOOP; i++) {
        ssize_t len = this->udp_socket_->read(buf, sizeof(buf));
        if (len <= 0) {
          break;
        }
        ESP_LOGD(TAG, "receive data, length=%d, data=%.*s", len, (int) len, (char *) buf);
        this->receive_string_buffer_.append((char *) buf, len);
        ESP_LOGD(TAG, "current buffer data=%s", this->receive_string_buffer_.c_str());
        this->fire_triggers();
      }
    }

    void LoxoneComponent::ensure_listen_tcp() {
      if (this->protocol_ != "tcp") {
        return;
      }

      if (this->server_ready_) {
        return;
      }

      this->tcp_listen_socket_ = socket::socket_ip(SOCK_STREAM, IPPROTO_TCP);
      if (this->tcp_listen_socket_ == nullptr) {
        ESP_LOGW(TAG, "could not create tcp listen socket");
        return;
      }
      this->tcp_listen_socket_->setblocking(false);
      int enable = 1;
      this->tcp_listen_socket_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

      struct sockaddr_storage bind_addr;
      socklen_t addr_len = socket::set_sockaddr_any((struct sockaddr *) &bind_addr, sizeof(bind_addr), this->listen_port_);
      if (this->tcp_listen_socket_->bind((struct sockaddr *) &bind_addr, addr_len) != 0) {
        ESP_LOGW(TAG, "tcp bind to port %u failed: errno %d", this->listen_port_, errno);
        this->tcp_listen_socket_ = nullptr;
        return;
      }
      if (this->tcp_listen_socket_->listen(4) != 0) {
        ESP_LOGW(TAG, "tcp listen failed: errno %d", errno);
        this->tcp_listen_socket_ = nullptr;
        return;
      }

      this->server_ready_ = true;
      ESP_LOGD(TAG, "listening on tcp port %u", this->listen_port_);
    }

    void LoxoneComponent::poll_tcp_server() {
      if (this->protocol_ != "tcp" || this->tcp_listen_socket_ == nullptr) {
        return;
      }

      // accept pending connections
      for (uint8_t i = 0; i < MAX_PACKETS_PER_LOOP; i++) {
        auto client = this->tcp_listen_socket_->accept(nullptr, nullptr);
        if (client == nullptr) {
          break;
        }
        client->setblocking(false);
        ESP_LOGD(TAG, "new client connected");
        this->tcp_server_clients_.push_back(std::move(client));
      }

      // read from connected clients
      uint8_t buf[1024];
      for (size_t c = 0; c < this->tcp_server_clients_.size(); c++) {
        auto &client = this->tcp_server_clients_[c];
        bool dead = false;
        for (uint8_t i = 0; i < MAX_PACKETS_PER_LOOP; i++) {
          ssize_t len = client->read(buf, sizeof(buf));
          if (len == 0) {
            // orderly shutdown by peer
            dead = true;
            break;
          }
          if (len < 0) {
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
              dead = true;
            }
            break;
          }
          ESP_LOGD(TAG, "receive data, length=%d, data=%.*s", len, (int) len, (char *) buf);
          this->receive_string_buffer_.append((char *) buf, len);
          ESP_LOGD(TAG, "current buffer data=%s", this->receive_string_buffer_.c_str());
          this->fire_triggers();
        }
        if (dead) {
          ESP_LOGD(TAG, "client disconnected");
          this->tcp_server_clients_.erase(this->tcp_server_clients_.begin() + c);
          c--;
        }
      }
    }

    void LoxoneComponent::ensure_connect_tcp() {
      if (this->protocol_ != "tcp") {
        return;
      }

      if (this->client_ready_) {
        return;
      }

      if (this->tcp_connecting_) {
        // re-issuing connect() on a connecting socket tells us the outcome:
        // errno EISCONN means established, EALREADY/EINPROGRESS means still connecting
        struct sockaddr_storage dest_addr;
        socklen_t addr_len = socket::set_sockaddr((struct sockaddr *) &dest_addr, sizeof(dest_addr), this->loxone_ip_, this->loxone_port_);
        int err = this->tcp_client_socket_->connect((struct sockaddr *) &dest_addr, addr_len);
        if (err == 0 || errno == EISCONN) {
          ESP_LOGD(TAG, "client connected");
          this->client_ready_ = true;
          this->tcp_connecting_ = false;
        } else if (errno == EALREADY || errno == EINPROGRESS || errno == EWOULDBLOCK) {
          ESP_LOGD(TAG, "client still connecting");
        } else {
          ESP_LOGD(TAG, "client connect failed: errno %d", errno);
          this->tcp_client_socket_ = nullptr;
          this->tcp_connecting_ = false;
        }
        return;
      }

      uint32_t now = millis();
      if (now - this->last_tcp_connect_attempt_ < TCP_CONNECT_RETRY_INTERVAL_MS) {
        return;
      }
      this->last_tcp_connect_attempt_ = now;

      this->tcp_client_socket_ = socket::socket_ip(SOCK_STREAM, IPPROTO_TCP);
      if (this->tcp_client_socket_ == nullptr) {
        ESP_LOGW(TAG, "could not create tcp client socket");
        return;
      }
      this->tcp_client_socket_->setblocking(false);

      struct sockaddr_storage dest_addr;
      socklen_t addr_len = socket::set_sockaddr((struct sockaddr *) &dest_addr, sizeof(dest_addr), this->loxone_ip_, this->loxone_port_);
      int err = this->tcp_client_socket_->connect((struct sockaddr *) &dest_addr, addr_len);
      if (err == 0) {
        ESP_LOGD(TAG, "client connected");
        this->client_ready_ = true;
      } else if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
        ESP_LOGD(TAG, "client connecting...");
        this->tcp_connecting_ = true;
      } else {
        ESP_LOGD(TAG, "client connect failed: errno %d", errno);
        this->tcp_client_socket_ = nullptr;
      }
    }

    void LoxoneComponent::poll_tcp_client() {
      if (this->protocol_ != "tcp" || !this->client_ready_ || this->tcp_client_socket_ == nullptr) {
        return;
      }

      uint8_t buf[1024];
      for (uint8_t i = 0; i < MAX_PACKETS_PER_LOOP; i++) {
        ssize_t len = this->tcp_client_socket_->read(buf, sizeof(buf));
        if (len == 0) {
          ESP_LOGD(TAG, "server closed connection");
          this->tcp_client_socket_ = nullptr;
          this->client_ready_ = false;
          return;
        }
        if (len < 0) {
          if (errno != EWOULDBLOCK && errno != EAGAIN) {
            ESP_LOGD(TAG, "client read error: errno %d", errno);
            this->tcp_client_socket_ = nullptr;
            this->client_ready_ = false;
          }
          return;
        }
        ESP_LOGD(TAG, "receive data, length=%d, data=%.*s", len, (int) len, (char *) buf);
        this->receive_string_buffer_.append((char *) buf, len);
        ESP_LOGD(TAG, "current buffer data=%s", this->receive_string_buffer_.c_str());
        this->fire_triggers();
      }
    }

    void LoxoneComponent::fire_triggers() {
      if (this->delimiter_ == "") {
        return;
      }

      size_t pos;
      while ((pos = this->receive_string_buffer_.find(this->delimiter_)) != std::string::npos) {
        std::string command = this->receive_string_buffer_.substr(0, pos);
        this->receive_string_buffer_.erase(0, pos + this->delimiter_.length());

        if (!command.empty()) {
          for (auto &trigger : this->string_triggers_) {
            trigger->trigger(command);
          }
        }
      }
    }

    void LoxoneComponent::send_data(const std::string &data) {
      if (this->protocol_ == "udp") {
        if (this->udp_socket_ == nullptr) return;
        struct sockaddr_storage dest_addr;
        socklen_t addr_len = socket::set_sockaddr((struct sockaddr *) &dest_addr, sizeof(dest_addr), this->loxone_ip_, this->loxone_port_);
        //this->udp_socket_->sendto(data.c_str(), data.length(), 0, (struct sockaddr *) &dest_addr, addr_len);
        //this->udp_socket_->sendto(this->delimiter_.c_str(), this->delimiter_.length(), 0, (struct sockaddr *) &dest_addr, addr_len);
        std::string packet = data + this->delimiter_;
        this->udp_socket_->sendto(packet.c_str(), packet.length(), 0, (struct sockaddr *) &dest_addr, addr_len);
      } else if (this->protocol_ == "tcp") {
        if (this->tcp_client_socket_ == nullptr) return;
        ssize_t written = this->tcp_client_socket_->write(data.c_str(), data.length());
        if (written < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
          ESP_LOGD(TAG, "client write error: errno %d", errno);
          this->tcp_client_socket_ = nullptr;
          this->client_ready_ = false;
          return;
        }
        this->tcp_client_socket_->write(this->delimiter_.c_str(), this->delimiter_.length());
      }
    }

    void LoxoneComponent::flush_send_buffer() {
      while (this->client_ready_ && !this->send_string_buffer_.empty()) {
        std::string d = this->send_string_buffer_.front();
        this->send_data(d);
        ESP_LOGD(TAG, "pop from queue, string data: %s", d.c_str());
        this->send_string_buffer_.pop();
      }
    }

    void LoxoneComponent::loop() {
      if (!network::is_connected()) {
        if (this->server_ready_ || this->client_ready_ || this->udp_socket_ != nullptr ||
            this->tcp_listen_socket_ != nullptr || this->tcp_client_socket_ != nullptr) {
          ESP_LOGD(TAG, "network not ready, tearing down sockets");
          this->teardown_();
        }
        return;
      }

      this->ensure_listen_udp();
      this->ensure_listen_tcp();
      this->ensure_connect_tcp();

      this->poll_udp();
      this->poll_tcp_server();
      this->poll_tcp_client();

      this->flush_send_buffer();
    }

    void LoxoneComponent::send_string_data(std::string data) {
      if (!this->client_ready_) {
        if (this->send_string_buffer_.size() >= this->send_buffer_length_) {
          ESP_LOGW(TAG, "send buffer is full, discarding some data");
          this->send_string_buffer_.pop();
        }

        this->send_string_buffer_.push(data);
        ESP_LOGD(TAG, "client is not ready, push into buffer, string data: %s", data.c_str());
        return;
      }

      this->send_data(data);
      ESP_LOGD(TAG, "send string data: %s", data.c_str());
    }
  }
}
