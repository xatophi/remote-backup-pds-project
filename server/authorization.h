#ifndef SERVER_PROGETTO_AUTHORIZATION_H
#define SERVER_PROGETTO_AUTHORIZATION_H

#include <fstream>
#include <sstream>
#include <string>
#include <optional>

#include "dao.h"

// return the username if is a valid token, {} otherwise
std::optional<std::string> verifyToken(const std::string& token);

// return true if the username and password match the db
bool verifyUserPassword(const std::string& username, const std::string& password);

// return a new token
std::string createToken(int n);

// save token into database
bool saveTokenToUser(std::string &username, std::string &token);

// logout user (delete token from database)
bool logoutUser(std::string &username);

// delete all tokens of users
void deleteAllTokens();

#endif //SERVER_PROGETTO_AUTHORIZATION_H