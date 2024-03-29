#ifndef PROTOCOL_FTP_RESPONSE_SRC_FTP_RESPONSE_HPP
#define PROTOCOL_FTP_RESPONSE_SRC_FTP_RESPONSE_HPP

#include <string>

#include "protocol/iserialize.hpp"

namespace protocol {

namespace ftp {

namespace response {

/**
 * \brief FTP response codes.
 *
 * As defined in: RFC 959 - File Transfer Protocol
 */
enum class FtpReplyCode {

  COMMAND_OK = 200,
  SYNTAX_ERROR_UNRECOGNIZED_COMMAND = 500,
  SYNTAX_ERROR_PARAMETERS = 501,
  COMMAND_NOT_IMPLEMENTED_SUPERFLOUS = 202,
  COMMAND_NOT_IMPLEMENTED = 502,
  COMMANDS_BAD_SEQUENCE = 503,
  COMMAND_NOT_IMPLEMENTED_FOR_PARAMETER = 504,

  REPLY_RESTART_MARKER = 110,
  REPLY_SYSTEM_STATUS = 211,
  DIRECTORY_STATUS = 212,
  FILE_STATUS = 213,
  HELP_MESSAGE = 214,
  NAME_SYSTEM_TYPE = 215,

  SERVICE_READY_IN_MINUTES = 120,
  SERVICE_READY_FOR_NEW_USER = 220,
  SERVICE_CLOSING_CONTROL_CONNECTION = 221,
  SERVICE_NOT_AVAILABLE = 421,
  DATA_CONNECTION_OPEN_TRANSFER_STARTING = 125,
  DATA_CONNECTION_OPEN_NO_TRANSFER = 225,
  ERROR_OPENING_DATA_CONNECTION = 425,
  CLOSING_DATA_CONNECTION = 226,
  TRANSFER_ABORTED = 426,
  ENTERING_PASSIVE_MODE = 227,

  USER_LOGGED_IN = 230,
  NOT_LOGGED_IN = 530,
  USER_NAME_OK = 331,
  NEED_ACCOUNT_FOR_LOGIN = 332,
  NEED_ACCOUNT_FOR_STORING_FILES = 532,
  FILE_STATUS_OK_OPENING_DATA_CONNECTION = 150,
  FILE_ACTION_COMPLETED = 250,
  PATHNAME_CREATED = 257,
  FILE_ACTION_NEEDS_FURTHER_INFO = 350,
  FILE_ACTION_NOT_TAKEN = 450,
  ACTION_NOT_TAKEN = 550,
  ACTION_ABORTED_LOCAL_ERROR = 451,
  ACTION_ABORTED_PAGE_TYPE_UNKNOWN = 551,
  ACTION_NOT_TAKEN_INSUFFICIENT_STORAGE_SPACE = 452,
  FILE_ACTION_ABORTED = 552,
  ACTION_NOT_TAKEN_FILENAME_NOT_ALLOWED = 553,
};

/**
 * \brief FTP reply.
 */
class FtpResponse : public ISerialize {
 public:
  /**
   * \brief Create FTP reply with the given code and message.
   *
   * \param code FTP reply code.
   * \param text FTP reply text.
   */
  FtpResponse(FtpReplyCode code, const std::string& text) noexcept
      : code_{code}, text_{text} {}

  /**
   * \brief Create FTP reply with the given code.
   *
   * \param code FTP reply code.
   */
  explicit FtpResponse(FtpReplyCode code) noexcept : code_{code}, text_{""} {}

  /**
   * \brief Convert FTP reply to string.
   *
   * \return String representation of FTP reply.
   */
  inline explicit operator std::string() const override {
    return std::to_string(static_cast<int>(code_)) + ' ' + text_ + "\r\n";
  }

 private:
  const FtpReplyCode code_;  ///< FTP reply code.
  const std::string text_;   ///< FTP reply text.
};

}  // namespace response
}  // namespace ftp
}  // namespace protocol

#endif  // PROTOCOL_FTP_RESPONSE_SRC_FTP_RESPONSE_HPP
