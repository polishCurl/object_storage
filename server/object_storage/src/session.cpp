
#include "session.hpp"

#include <boost/log/trivial.hpp>

#include "protocol/detector/src/protocol_detector.hpp"
#include "protocol/ftp/response/src/ftp_response.hpp"
#include "protocol/http/response/src/http_response.hpp"

using ErrorCode = boost::system::error_code;
using protocol::detector::AppLayerProtocol;
using protocol::detector::detectProtocol;
using protocol::ftp::request::FtpCommand;
using protocol::ftp::request::FtpParser;
using protocol::ftp::response::FtpReplyCode;
using protocol::ftp::response::FtpResponse;
using protocol::http::request::HttpMethod;
using protocol::http::request::HttpParser;
using protocol::http::response::HttpResource;
using protocol::http::response::HttpResponse;
using protocol::http::response::HttpResponseHeaders;
using protocol::http::response::HttpStatus;
using user::User;

namespace server {
namespace object_storage {

Session::Session(IOService& io_service, const user::UserDatabase& user_database,
                 bool authenticate, fs::MemoryFs& filesystem,
                 PortRange ftp_port_range,
                 const std::function<void()>& completion_handler)
    :  // ------------------ COMMON ------------------
      completion_handler_{completion_handler},
      user_database_{user_database},
      authenticate_{authenticate},
      filesystem_{filesystem},
      io_service_{io_service},
      socket_{io_service_},
      serializer_{io_service_},
      // ------------------ FTP ------------------
      ftp_data_acceptor_{io_service},
      ftp_data_serializer_{io_service},
      ftp_port_range_{ftp_port_range},
      last_ftp_command_{FtpCommand::Unrecognized},
      ftp_handlers_{
          {FtpCommand::User,
           std::bind(&Session::handleFtpUser, this, std::placeholders::_1)},
          {FtpCommand::Pass,
           std::bind(&Session::handleFtpPass, this, std::placeholders::_1)},
          {FtpCommand::List,
           std::bind(&Session::handleFtpList, this, std::placeholders::_1)},
          {FtpCommand::Retr,
           std::bind(&Session::handleFtpRetr, this, std::placeholders::_1)},
          {FtpCommand::Stor,
           std::bind(&Session::handleFtpStor, this, std::placeholders::_1)},
          {FtpCommand::Dele,
           std::bind(&Session::handleFtpDele, this, std::placeholders::_1)},
          {FtpCommand::Pasv,
           std::bind(&Session::handleFtpPasv, this, std::placeholders::_1)},
          {FtpCommand::Type,
           std::bind(&Session::handleFtpType, this, std::placeholders::_1)},
      },
      // ------------------ HTTP ------------------
      http_handlers_{
          {HttpMethod::Get,
           std::bind(&Session::handleHttpGet, this, std::placeholders::_1)},
          {HttpMethod::Put,
           std::bind(&Session::handleHttpPut, this, std::placeholders::_1)},
          {HttpMethod::Delete,
           std::bind(&Session::handleHttpDelete, this, std::placeholders::_1)},
      } {}

Session::~Session() {
  closeSocket();
  closeFtpDataSocket();
  completion_handler_();
}

void Session::start() noexcept {
  BOOST_LOG_TRIVIAL(debug) << "Starting session with "
                           << getRemoteEndpointInfo();
  setTcpNoDelay();
  serializer_.post(
      [me = shared_from_this()]() { me->receiveMessageHandler(); });

  // Send 'ready for new user' message only if we know that the client is FTP.
  const auto client_port = socket_.remote_endpoint().port();
  if ((client_port >= ftp_port_range_.min_port) &&
      (client_port <= ftp_port_range_.max_port))
    sendMessage(FtpResponse(FtpReplyCode::SERVICE_READY_FOR_NEW_USER,
                            "Welcome to ObjectStorage server"));
}

void Session::setTcpNoDelay() noexcept {
  ErrorCode error_code;
  socket_.set_option(boost::asio::ip::tcp::no_delay(true), error_code);
  if (error_code)
    BOOST_LOG_TRIVIAL(error)
        << "Failed to set HTTP/FTP socket option TCP_NODELAY:"
        << error_code.message();
}

void Session::closeSocket() noexcept {
  ErrorCode error_code;
  socket_.shutdown(Socket::shutdown_both, error_code);
  socket_.close(error_code);
}

std::string Session::getRemoteEndpointInfo() const noexcept {
  return socket_.remote_endpoint().address().to_string() + ":" +
         std::to_string(socket_.remote_endpoint().port());
}

void Session::receiveMessageHandler() noexcept {
  // Asynchronously read input request until CRLF symbol is found. The CRLF is
  // shared between HTTP (end of request line) and FTP (end of entire
  // request).
  boost::asio::async_read_until(
      socket_, input_stream_, "\r\n",
      serializer_.wrap([me = shared_from_this()](ErrorCode error_code,
                                                 std::size_t header_length) {
        if (error_code) {
          if (error_code == boost::asio::error::eof) {
            BOOST_LOG_TRIVIAL(info)
                << "Connection closed by " << me->getRemoteEndpointInfo();
          } else {
            BOOST_LOG_TRIVIAL(error)
                << "Failed to read HTTP/FTP request header: "
                << error_code.message();
          }

          me->ftp_data_acceptor_.close(error_code);
          me->ftp_data_serializer_.post([me]() { me->closeFtpDataSocket(); });

        } else {
          // Process packet to determine if if is HTTP or FTP.
          std::istream stream(&(me->input_stream_));
          std::string packet;
          packet.resize(header_length);
          stream.read(&packet[0], header_length);
          BOOST_LOG_TRIVIAL(debug) << "Received message:\n" << packet;
          const auto app_layer_protocol = detectProtocol(packet);

          switch (app_layer_protocol) {
            case AppLayerProtocol::Http:
              me->handleHttpRequest(packet);
              break;

            case AppLayerProtocol::Ftp:
              me->handleFtpRequest(packet);
              break;
          }

          me->receiveMessageHandler();
        }
      }));
}

void Session::sendMessageHandler() noexcept {
  BOOST_LOG_TRIVIAL(debug) << "Sending message:\n" << output_queue_.front();

  // Get the next message from send queue and send it asynchronously.
  boost::asio::async_write(
      socket_, boost::asio::buffer(output_queue_.front()),
      serializer_.wrap(
          [me = shared_from_this()](ErrorCode error_code, std::size_t) {
            if (!error_code) {
              me->output_queue_.pop_front();

              // If there are more messages to send, trigger this handler again.
              // Otherwise, the thread enqueueing a new message will trigger
              // this handler.
              const auto write_in_progress = !me->output_queue_.empty();
              if (write_in_progress) {
                me->sendMessageHandler();
              }
            } else {
              BOOST_LOG_TRIVIAL(error)
                  << "Failed to send message:\n"
                  << me->output_queue_.front() << error_code.message();
            }
          }));
}

void Session::sendMessage(const std::string& message) {
  // Put the message to the send queue. If the send queue was empty before
  // manually trigger the asynchronous send handler execution.
  serializer_.post([me = shared_from_this(), message]() {
    const bool write_in_progress = !me->output_queue_.empty();
    me->output_queue_.push_back(message);
    if (!write_in_progress) {
      me->sendMessageHandler();
    }
  });
}

void Session::closeFtpDataSocket() noexcept {
  ErrorCode error_code;
  auto data_socket = ftp_data_socket_.lock();
  if (data_socket) {
    data_socket->shutdown(Socket::shutdown_both, error_code);
    data_socket->close(error_code);
  }
}

void Session::sendFtpDataHandler(
    const std::shared_ptr<Socket>& data_socket) noexcept {
  ftp_data_serializer_.post([me = shared_from_this(), data_socket]() {
    // Get the next file from FTP data socket send queue.
    const auto data = me->ftp_data_buffer_.front();

    if (data) {
      // If the file is non-empty, send if over FTP data socket and retrigger
      // this handler so that the next file can be sent.
      boost::asio::async_write(
          *data_socket, boost::asio::buffer(*data),
          me->ftp_data_serializer_.wrap(
              [me, data, data_socket](ErrorCode error_code, std::size_t) {
                me->ftp_data_buffer_.pop_front();

                if (error_code) {
                  BOOST_LOG_TRIVIAL(error)
                      << "Failed to write data: " << error_code.message();
                  return;
                }

                if (!me->ftp_data_buffer_.empty()) {
                  me->sendFtpDataHandler(data_socket);
                }
              }));
    } else {
      // If the file to send is empty, this means transmission end.
      // Close the FTP data socket.
      me->ftp_data_buffer_.pop_front();
      ErrorCode error_code;
      data_socket->shutdown(Socket::shutdown_both, error_code);
      data_socket->close(error_code);

      me->sendMessage(
          FtpResponse(FtpReplyCode::CLOSING_DATA_CONNECTION, "Done"));
    }
  });
}

void Session::enqueueFtpDataHandler(
    const std::shared_ptr<fs::File>& data,
    const std::shared_ptr<Socket>& data_socket) noexcept {
  ftp_data_serializer_.post([me = shared_from_this(), data, data_socket]() {
    //
    const auto write_in_progress = (!me->ftp_data_buffer_.empty());
    me->ftp_data_buffer_.push_back(data);
    if (!write_in_progress) {
      me->sendFtpDataHandler(data_socket);
    }
  });
}

void Session::handleFtpRequest(const std::string& request) noexcept {
  // If request is valid, delegate it to the right handler based on the FTP
  // command.
  FtpParser parser{request};
  if (parser.isValid()) {
    const auto ftp_command = parser.getCommand();
    ftp_handlers_.at(ftp_command)(parser);
    last_ftp_command_ = ftp_command;
  } else {
    sendMessage(FtpResponse(FtpReplyCode::SYNTAX_ERROR_UNRECOGNIZED_COMMAND));
  }
}

void Session::handleFtpUser(const protocol::ftp::request::FtpParser& parser) {
  // Store the username until the PASS command is received.
  logged_in_user_.reset();
  last_username_ = parser.getTokens()[1];
  sendMessage(
      FtpResponse(FtpReplyCode::USER_NAME_OK, "Please provide password"));
}

void Session::handleFtpPass(const protocol::ftp::request::FtpParser& parser) {
  if (last_ftp_command_ != FtpCommand::User) {
    sendMessage(FtpResponse(FtpReplyCode::COMMANDS_BAD_SEQUENCE,
                            "Please specify username first"));
  } else {
    // Store the username until the PASS command is received.
    const User user{last_username_, std::string{parser.getTokens()[1]}};
    if (user_database_.exists(user)) {
      logged_in_user_ = user;
      sendMessage(
          FtpResponse(FtpReplyCode::USER_LOGGED_IN, "Login successful"));
    } else {
      sendMessage(FtpResponse(FtpReplyCode::NOT_LOGGED_IN, "Failed to log in"));
    }
  }
}

void Session::handleFtpList(const protocol::ftp::request::FtpParser& parser) {
  if (!logged_in_user_) {
    sendMessage(FtpResponse(FtpReplyCode::NOT_LOGGED_IN, "Not logged in"));
  }

  sendMessage(FtpResponse(FtpReplyCode::FILE_STATUS_OK_OPENING_DATA_CONNECTION,
                          "Listing all objects stored"));

  // Serialize the file list into a single text file.
  const auto filenames = filesystem_.list();
  auto list_file = std::make_shared<fs::File>();
  for (const auto& filename : filenames) {
    *list_file += filename + '\n';
  }

  // Asynchronous put the file into the FTP data send queue.
  auto data_socket = std::make_shared<Socket>(io_service_);
  ftp_data_acceptor_.async_accept(
      *data_socket,
      ftp_data_serializer_.wrap(
          [data_socket, list_file, me = shared_from_this()](auto error_code) {
            if (error_code) {
              me->sendMessage(FtpResponse(
                  FtpReplyCode::TRANSFER_ABORTED,
                  "Data transfer aborted: " + error_code.message()));
              return;
            }

            me->ftp_data_socket_ = data_socket;
            me->enqueueFtpDataHandler(list_file, data_socket);
            me->enqueueFtpDataHandler({}, data_socket);
          }));
}

void Session::handleFtpRetr(const protocol::ftp::request::FtpParser& parser) {}

void Session::handleFtpStor(const protocol::ftp::request::FtpParser& parser) {}

void Session::handleFtpDele(const protocol::ftp::request::FtpParser& parser) {
  if (!logged_in_user_) {
    sendMessage(FtpResponse(FtpReplyCode::NOT_LOGGED_IN, "Not logged in"));
  }

  const auto& filename = std::string{parser.getTokens()[1]};
  const auto status = filesystem_.remove(filename);
  switch (status) {
    case fs::Status::Success:
      BOOST_LOG_TRIVIAL(info) << "Deleted file: " << filename;
      sendMessage(
          FtpResponse(FtpReplyCode::FILE_ACTION_COMPLETED, "File deleted"));
      break;
    default:
      sendMessage(
          FtpResponse(FtpReplyCode::ACTION_NOT_TAKEN, "Unable to delete file"));

      break;
  }
}

void Session::handleFtpPasv(const protocol::ftp::request::FtpParser& parser) {
  if (!logged_in_user_) {
    sendMessage(FtpResponse(FtpReplyCode::NOT_LOGGED_IN, "Not logged in"));
  }

  else if (!setUpFtpDataConnectionAcceptor()) {
    sendMessage(FtpResponse(FtpReplyCode::SERVICE_NOT_AVAILABLE,
                            "Passive mode not supported"));
  }

  else {
    // Respond to FTP client with IP address and port for data connection.
    const auto ip_bytes = socket_.local_endpoint().address().to_v4().to_bytes();
    const auto port = ftp_data_acceptor_.local_endpoint().port();

    std::stringstream stream;
    stream << "(";
    for (const auto byte : ip_bytes) {
      stream << static_cast<unsigned int>(byte) << ",";
    }
    stream << ((port >> 8) & 0xff) << "," << (port & 0xff) << ")";
    sendMessage(FtpResponse(FtpReplyCode::ENTERING_PASSIVE_MODE,
                            "Entering passive mode " + stream.str()));
  }
}

void Session::handleFtpType(const protocol::ftp::request::FtpParser& parser) {
  if (!logged_in_user_) {
    sendMessage(FtpResponse(FtpReplyCode::NOT_LOGGED_IN, "Not logged in"));
  } else {
    sendMessage(FtpResponse(FtpReplyCode::COMMAND_OK, "Mode switched"));
  }
}

bool Session::setUpFtpDataConnectionAcceptor() noexcept {
  ErrorCode error_code;
  if (ftp_data_acceptor_.is_open()) {
    ftp_data_acceptor_.close(error_code);
    if (error_code) {
      BOOST_LOG_TRIVIAL(error)
          << "Error closing data acceptor: " << error_code.message();
    }
  }

  const Endpoint endpoint{boost::asio::ip::tcp::v4(), 0};

  ftp_data_acceptor_.open(endpoint.protocol(), error_code);
  if (error_code) {
    BOOST_LOG_TRIVIAL(error)
        << "Failed to open FTP data acceptor: " << error_code.message();

    return false;
  }

  ftp_data_acceptor_.bind(endpoint, error_code);
  if (error_code) {
    BOOST_LOG_TRIVIAL(error)
        << "Failed to bind FTP data acceptor: " << error_code.message();
    return false;
  }

  ftp_data_acceptor_.listen(boost::asio::socket_base::max_connections,
                            error_code);
  if (error_code) {
    BOOST_LOG_TRIVIAL(error) << "Failed to listen for FTP data connections: "
                             << error_code.message();
    return false;
  }

  return true;
}

void Session::handleHttpRequest(std::string& request) noexcept {
  auto status_line_size = request.size();

  // Read the rest of the HTTP request (excluding message body)
  auto response_headers_size =
      boost::asio::read_until(socket_, input_stream_, "\r\n\r\n");
  std::istream stream(&input_stream_);
  request.resize(request.size() + response_headers_size);
  stream.read(&request[status_line_size], response_headers_size);

  BOOST_LOG_TRIVIAL(debug) << "HTTP request:\n" << request;

  // If request is valid, delegate it to the right handler based on the HTTP
  // method.
  HttpParser parser{request};
  if (!parser.isValid()) {
    BOOST_LOG_TRIVIAL(error) << "Failed to parse HTTP request:\n" << request;
    sendMessage(HttpResponse{HttpStatus::BadRequest});

  } else if (!authenticateHttpUser(parser)) {
    sendMessage(HttpResponse{HttpStatus::Unauthorized,
                             HttpResponseHeaders{{"WWW-Authenticate", "Basic"},
                                                 {"Content-Length", "0"}}});
  } else {
    http_handlers_.at(parser.getMethod())(parser);
  }
}

bool Session::authenticateHttpUser(const HttpParser& parser) noexcept {
  if (authenticate_) {
    const auto auth_info = parser.getAuthInfo();
    user::User user_to_auth{auth_info->username, auth_info->password};
    const auto user_exists = user_database_.exists(user_to_auth);

    if (!user_exists) {
      BOOST_LOG_TRIVIAL(error)
          << "Failed to authenticate user " << user_to_auth;
    }

    return user_exists;
  }

  return true;
}

void Session::handleHttpGet(const HttpParser& parser) {
  if (parser.getUri() == "/") {
    // If request has 'GET /' format, list all files stored in the filesystem.
    const auto filenames = filesystem_.list();
    std::string response;
    for (const auto& file : filenames) {
      response += file + '\n';
    }
    sendMessage(HttpResponse{HttpStatus::Ok, response});

  } else {
    // Otherwise, get the file from the filesystem and send it in response (if
    // it was found).
    const auto [status, file] = filesystem_.get(std::string{parser.getUri()});
    switch (status) {
      case fs::Status::Success:
        sendMessage(HttpResponse{HttpStatus::Ok, file});
        break;
      case fs::Status::FileNotFound:
        sendMessage(HttpResponse{HttpStatus::NotFound});
        break;
      default:
        sendMessage(HttpResponse{HttpStatus::InternalServerError});
        break;
    }
  }
}

void Session::handleHttpPut(const HttpParser& parser) {
  ErrorCode error_code;
  const auto file_size = parser.getResourceSize();
  const auto filename = std::string{parser.getUri()};

  // Read the HTTP message body containing the content/resource/file.
  auto bytes_read =
      boost::asio::read(socket_, input_stream_,
                        boost::asio::transfer_exactly(file_size), error_code);

  if (bytes_read != file_size) {
    BOOST_LOG_TRIVIAL(error) << "Failed to read " << file_size
                             << " bytes (Actual: " << bytes_read << ')';
    sendMessage(HttpResponse{HttpStatus::BadRequest});
  } else {
    auto bufs = input_stream_.data();
    fs::File file(boost::asio::buffers_begin(bufs),
                  boost::asio::buffers_begin(bufs) + file_size);

    const auto status = filesystem_.add(std::string{filename}, file);
    switch (status) {
      case fs::Status::Success:
        BOOST_LOG_TRIVIAL(info) << "Saved file: " << filename;
        sendMessage(HttpResponse{HttpStatus::Created});
        break;
      case fs::Status::AlreadyExists:
        sendMessage(HttpResponse{HttpStatus::NotFound});
        break;
      default:
        sendMessage(HttpResponse{HttpStatus::InternalServerError});
        break;
    }
  }
}

void Session::handleHttpDelete(const HttpParser& parser) {
  const auto& filename = std::string{parser.getUri()};
  const auto status = filesystem_.remove(filename);
  switch (status) {
    case fs::Status::Success:
      BOOST_LOG_TRIVIAL(info) << "Deleted file: " << filename;
      sendMessage(HttpResponse{HttpStatus::Ok});
      break;
    case fs::Status::FileNotFound:
      sendMessage(HttpResponse{HttpStatus::NotFound});
      break;
    default:
      sendMessage(HttpResponse{HttpStatus::InternalServerError});
      break;
  }
}

}  // namespace object_storage
}  // namespace server
