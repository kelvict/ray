#include "client_connection.h"

#include <boost/bind.hpp>

#include "common.h"
#include "ray/raylet/format/node_manager_generated.h"

namespace ray {

ray::Status TcpConnect(boost::asio::ip::tcp::socket &socket,
                       const std::string &ip_address_string, int port) {
  boost::asio::ip::address ip_address =
      boost::asio::ip::address::from_string(ip_address_string);
  boost::asio::ip::tcp::endpoint endpoint(ip_address, port);
  boost::system::error_code error;
  socket.connect(endpoint, error);
  if (error) {
    return ray::Status::IOError(error.message());
  } else {
    return ray::Status::OK();
  }
}

template <class T>
ServerConnection<T>::ServerConnection(boost::asio::basic_stream_socket<T> &&socket)
    : socket_(std::move(socket)) {}

template <class T>
ray::Status ServerConnection<T>::WriteMessage(int64_t type, size_t length,
                                              const uint8_t *message) {
  std::vector<boost::asio::const_buffer> message_buffers;
  auto write_version = RayConfig::instance().ray_protocol_version();
  message_buffers.push_back(boost::asio::buffer(&write_version, sizeof(write_version)));
  message_buffers.push_back(boost::asio::buffer(&type, sizeof(type)));
  message_buffers.push_back(boost::asio::buffer(&length, sizeof(length)));
  message_buffers.push_back(boost::asio::buffer(message, length));
  // Write the message and then wait for more messages.
  // TODO(swang): Does this need to be an async write?
  boost::system::error_code error;
  boost::asio::write(socket_, message_buffers, error);
  if (error) {
    return ray::Status::IOError(error.message());
  } else {
    return ray::Status::OK();
  }
}

// TODO(hme): Get rid of this.
template <class T>
boost::asio::basic_stream_socket<T> &ServerConnection<T>::GetSocket(){
  return socket_;
}

template <class T>
std::shared_ptr<ClientConnection<T>> ClientConnection<T>::Create(
    ClientHandler<T> &client_handler, MessageHandler<T> &message_handler,
    boost::asio::basic_stream_socket<T> &&socket) {
  std::shared_ptr<ClientConnection<T>> self(
      new ClientConnection(message_handler, std::move(socket)));
  // Let our manager process our new connection.
  client_handler(self);
  return self;
}

template <class T>
ClientConnection<T>::ClientConnection(MessageHandler<T> &message_handler,
                                      boost::asio::basic_stream_socket<T> &&socket)
    : ServerConnection<T>(std::move(socket)), message_handler_(message_handler) {}

template <class T>
void ClientConnection<T>::ProcessMessages() {
  // Wait for a message header from the client. The message header includes the
  // protocol version, the message type, and the length of the message.
  std::vector<boost::asio::mutable_buffer> header;
  header.push_back(boost::asio::buffer(&read_version_, sizeof(read_version_)));
  header.push_back(boost::asio::buffer(&read_type_, sizeof(read_type_)));
  header.push_back(boost::asio::buffer(&read_length_, sizeof(read_length_)));
  boost::asio::async_read(
      ServerConnection<T>::socket_, header,
      boost::bind(&ClientConnection<T>::ProcessMessageHeader, this->shared_from_this(),
                  boost::asio::placeholders::error));
}

template <class T>
void ClientConnection<T>::ProcessMessageHeader(const boost::system::error_code &error) {
  if (error) {
    // If there was an error, disconnect the client.
    read_type_ = MessageType_DisconnectClient;
    read_length_ = 0;
    ProcessMessage(error);
    return;
  }

  // If there was no error, make sure the protocol version matches.
  RAY_CHECK(read_version_ == RayConfig::instance().ray_protocol_version());
  // Resize the message buffer to match the received length.
  read_message_.resize(read_length_);
  // Wait for the message to be read.
  boost::asio::async_read(
      ServerConnection<T>::socket_, boost::asio::buffer(read_message_),
      boost::bind(&ClientConnection<T>::ProcessMessage, this->shared_from_this(),
                  boost::asio::placeholders::error));
}

template <class T>
void ClientConnection<T>::ProcessMessage(const boost::system::error_code &error) {
  if (error) {
    // TODO(hme): Disconnect differently & remove dependency on node_manager_generated.h
    read_type_ = MessageType_DisconnectClient;
  }
  message_handler_(this->shared_from_this(), read_type_, read_message_.data());
}

template class ServerConnection<boost::asio::local::stream_protocol>;
template class ServerConnection<boost::asio::ip::tcp>;
template class ClientConnection<boost::asio::local::stream_protocol>;
template class ClientConnection<boost::asio::ip::tcp>;

}  // namespace ray
