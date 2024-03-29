#ifndef SERVER_PROGETTO_SERVER_H
#define SERVER_PROGETTO_SERVER_H

#include <boost/beast/core/detail/base64.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/config.hpp>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>

#include "backup.h"
#include "authorization.h"

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>
using json = nlohmann::json;
namespace base64 = boost::beast::detail::base64;

// Report a failure
void fail(beast::error_code ec, const std::string  &what);

// replace %20 with the space
void replaceSpaces(std::string &str);


// This function produces an HTTP response for the given
// request. The type of the response object depends on the
// contents of the request, so the interface requires the
// caller to pass a generic lambda for receiving the response.
template<class Body, class Allocator, class Send>
void handle_request(http::request<Body, http::basic_fields<Allocator>>&& req,Send&& send){

    /*
    std::cout << "sleeping.." <<std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(5));
    */

    // Returns a bad request response
    auto const bad_request =
            [&req](const std::string &why){
                http::response<http::string_body> res{http::status::bad_request, req.version()};
                res.set(http::field::content_type, "text/plain");
                res.body() = why;
                res.prepare_payload();
                return res;
            };

    // Returns a not found response
    auto const not_found =
            [&req](){
                http::response<http::empty_body> res{http::status::not_found, req.version()};
                return res;
            };

    // Returns a server error response
    auto const server_error =
            [&req](const std::string &what){
                http::response<http::string_body> res{http::status::internal_server_error, req.version()};
                res.set(http::field::content_type, "text/plain");
                res.body() = "An error occurred: '" + what + "'";
                res.prepare_payload();
                return res;
            };

    auto const okay_response =
            [&req](){
                http::response<http::empty_body> res{http::status::ok, req.version()};
                return res;
            };

    auto const unauthorized_response =
            [&req](const std::string &what)
            {
                http::response<http::string_body> res{http::status::unauthorized, req.version()};
                res.set(http::field::content_type, "text/plain");
                res.body() = "Unauthorized: '" + what + "'";
                res.prepare_payload();
                return res;
            };

    // Make sure we can handle the method
    if( req.method() != http::verb::get &&
        req.method() != http::verb::post &&
        req.method() != http::verb::delete_)
        return send(bad_request("Unknown HTTP-method"));


    std::string req_path = req.target().to_string();

    //avoid path traversal
    if(req_path.find("..")!=std::string::npos)
        return send(bad_request("Bad path"));
    // substitute %20 with spaces
    replaceSpaces(req_path);

    // POST (login)
    if(req.method() == http::verb::post && req_path.rfind("/login", 0) == 0) {
        //login request
        json j = json::parse(req.body());

        std::string username;
        std::string password;

        try {
            username = j.at("username");
            password = j.at("password");
        }
        catch (json::exception &e) {
            //missing login parameters
            return send(bad_request("Missing login parameters"));
        }

        if (verifyUserPassword(username, password)) {
            // the user exists and the password is verified

            // create token
            std::string token = createToken(32);

            // save token related to user
            if (!saveTokenToUser(username, token))
                return send(server_error("Error in creating token to user"));

            // send token
            http::response<http::string_body> res{http::status::ok,req.version(),token};
            res.set(http::field::content_type, "text/plain");
            res.content_length(token.size());
            return send(std::move(res));
        } else {
            // if the verification of the password fails send error
            return send(unauthorized_response("Authentication failed"));
        }
    }

    //check if authorized
    auto auth = req[http::field::authorization];
    if(auth.empty()){
        //std::clog << "No token" << std::endl;
        return send(unauthorized_response("Token needed"));
    }
    std::string token = auth.to_string();
    std::optional<std::string> user = verifyToken(token);
    if (!user.has_value()) {
        //std::clog << "Invalid token: " + token << std::endl;
        return send(unauthorized_response("Invalid token"));
    }

    //POST (authenticated)
    if(req.method() == http::verb::post){
        //if starts with backup
        if (req_path.rfind("/backup/", 0) == 0){
            std::string path = req_path.substr(8);

            //std::clog << " post /backup " << path << std::endl;

            json j = json::parse(req.body());

            std::string type;
            try{
                type = j.at("type");
            } catch(json::exception & e){
                return send(bad_request("Missing parameters"));
            }

            if(type == "file") {
                std::string encodedfile;
                try{
                    encodedfile = j.at("encodedfile");
                } catch(json::exception & e){
                    return send(bad_request("Missing parameters"));
                }

                std::size_t max_l = base64::decoded_size(encodedfile.size());
                std::unique_ptr<char[]> raw_file{new char[max_l]};
                std::pair<std::size_t, std::size_t> res = base64::decode(raw_file.get(), encodedfile.c_str(), encodedfile.size());

                if (save_file(user.value(), path, std::move(raw_file), res.first)) {
                    //std::clog << " saved file " << path << std::endl;
                    return send(okay_response());
                } else {
                    //std::clog << "impossible save file " << path << std::endl;
                    return send(server_error("Impossible save the file, retry"));
                }

            } else if (type == "folder"){
                if(new_directory(user.value(),path)){
                    //std::clog << " saved folder " << path << std::endl;
                    return send(okay_response());
                } else {
                    //std::clog << "impossible save file " << path << std::endl;
                    return send(server_error("Impossible create the folder"));
                }
            } else {
                return send(bad_request("Illegal type"));
            }
        }

        if (req_path.rfind("/probefolder/", 0) == 0) {
            std::string path = req_path.substr(13);

            //std::clog << "post /probefolder " << path << std::endl;
            json j = json::parse(req.body());

            std::set<std::string> children;
            try{
                j.at("children").get_to(children);
            }catch(json::exception & e){
                return send(bad_request("Bad request body"));
            }


            bool res = probe_directory(user.value(),path,children);

            if(res){
                //folder exists
                //std::clog << "folder exists " << std::endl;
                return send(okay_response());
            } else {
                //folder not found
                //std::clog << "folder not found " << std::endl;
                return send(not_found());
            }
        }
        if (req_path.rfind("/logout", 0) == 0){
            if (logoutUser(user.value())) {
                return send(okay_response());
            } else {
                return send(server_error("Error during logout"));
            }
        }

        //all other POST requests
        return send(not_found());
    }

    // GET
    if(req.method() == http::verb::get) {
        //if starts with probefile
        if (req_path.rfind("/probefile/", 0) == 0) {
            std::string path = req_path.substr(11);
            //std::clog << "get /probefile " << path << std::endl;

            std::optional<std::string> digest_opt = get_file_digest(user.value(), path);

            if(digest_opt){
                //file exists
                std::string digest = digest_opt.value();
                http::response<http::string_body> res{http::status::ok, req.version(), digest};
                res.set(http::field::content_type, "text/plain");
                res.content_length(digest.size());
                return send(std::move(res));
            } else {
                //file not found
                return send(not_found());
            }
        }
        return send(not_found());
    }


    //DELETE
    if (req.method() == http::verb::delete_){
        //if starts with backup
        if (req_path.rfind("/backup/", 0) == 0){
            std::string path = req_path.substr(8);

            // avoid root cancellation
            if (path.empty()){
                return send(bad_request("Bad path"));
            }

            //std::clog << "delete request to "  << path << std::endl;

            if(backup_delete(user.value(),path)) {
                //std::clog << "delete ok "<< path << std::endl;
                return send(okay_response());
            }
            else {
                //std::clog << "delete fail " << path << std::endl;
                return send(not_found());
            }
        }
        return send(not_found());
    }
}


#endif //SERVER_PROGETTO_SERVER_H