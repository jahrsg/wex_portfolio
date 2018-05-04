// Copyright (c) 2015 Scruffy Scruffington
// Distributed under the Apache 2.0 software license, see the LICENSE file
#include "WexTradeApi.h"
#include "Log.h"
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string/join.hpp>
#include <chrono>
#include <thread>
#include <algorithm>
#include <list>
#include <iostream>

using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>
namespace ssl = boost::asio::ssl;       // from <boost/asio/ssl.hpp>
namespace http = boost::beast::http;    // from <boost/beast/http.hpp>
using namespace boost::property_tree;
using namespace std;

WexTradeApi::WexTradeApi(const std::string& key, const std::string& secret):
	m_key(key),
	m_secret(secret), 
	m_nonce(time(0))
{
    Log l("WexTradeApi::WexTradeApi()");
}

struct OrderChecker
{
    WexTradeApi* m_api;

    OrderChecker(WexTradeApi* api) : m_api(api) {}

    bool operator()(const pair<long long, std::string>& p)
	{
		return !m_api->checkOrder(p.first, p.second);
	}
};

bool WexTradeApi::execute(const std::vector<Order>& orders, unsigned timeout)
{
    Log l("WexTradeApi::execute");
	chrono::steady_clock::time_point start = chrono::steady_clock::now();
    list<pair<long long, std::string>> ids;
	for (const Order& o : orders)
		ids.push_back(make_pair(createOrder(o), o.coin));
	OrderChecker check(this);
	while (true)
	{
		if (ids.empty())
			break;
		ids.remove_if(check);
		if (chrono::steady_clock::now() - start > chrono::minutes(timeout))
			break;
		std::this_thread::sleep_for(chrono::seconds(30));
	}
	bool res = !ids.empty();
	for (auto p : ids)
        deleteOrder(p.first);
	return res;
}

std::string double_to_string(double val,  unsigned decimal_places)
{
    std::stringstream stream;
    stream << std::fixed << std::setprecision(decimal_places) << val;
    return stream.str();
}

long long WexTradeApi::createOrder(const Order& order)
{
    Log l("WexTradeApi::createOrder");
	std::map<std::string, std::string> params;
    params["method"] = "Trade";
    params["pair"] = order.coin + "_btc";
    params["type"] = (order.action == BUY) ? "buy" : "sell";
    params["rate"] = double_to_string(order.price, m_params[order.coin].decimal_places);
    params["amount"] = double_to_string(order.amount, m_params[order.coin].decimal_places);
    if(m_params[order.coin].reverted)
    {
        params["pair"] = "btc_" + order.coin;
        params["type"] = (order.action == BUY) ? "sell" : "buy";
        params["rate"] = double_to_string(1.0 / order.price, m_params[order.coin].decimal_places);
        params["amount"] = double_to_string(order.amount * order.price, m_params[order.coin].decimal_places);
    }
    std::istringstream is(call(params));
	ptree pt;
	read_json(is, pt);
	std::string err = pt.get("error", "");
    long long order_id = 0;
    if (!err.size())
    {
        ptree result = pt.get_child("return");
        order_id = result.get<long long>("order_id");
    }
    if (!m_log.empty())
	{
		ofstream fout(m_log, ofstream::app);
		time_t ttp = chrono::system_clock::to_time_t(chrono::system_clock::now());
		fout << "****" << std::ctime(&ttp) << "****" << endl;
		fout << "Create order: " << endl;
		fout << "Action: " << ((order.action == BUY) ? "buy" : "sell") << endl;
		fout << "Currency: " << order.coin << endl;
		fout << "Rate: " << order.price << endl;
		fout << "Amount: " << order.amount << endl;
		fout << "Result: ";
		if (err.size())
			fout << "error [" << err << "]" << endl;
		else
            fout << order_id << endl;
	}
    if (err.size())
    {
        Log::write("throw");
        throw std::runtime_error(err);
    }
    return order_id;
}

bool WexTradeApi::checkOrder(long long id, const std::string& coin)
{
    Log l(boost::str(boost::format("WexTradeApi::checkOrder(%d, %s)") %
                            id % coin.c_str()));
    if(id == 0)
        return false;
	std::map<std::string, std::string> params;
    params["method"] = "OrderInfo";
    params["order_id"] = boost::lexical_cast<std::string>(id);
    std::istringstream is(call(params));
	ptree pt;
	read_json(is, pt);
	std::string err = pt.get("error", "");
	if (err.size())
	{
		Log::write("throw");
		throw std::runtime_error(err);
	}
    ptree result = pt.get_child("return");
    for (ptree::iterator it = result.begin(); it != result.end(); ++it)
	{
		ptree cpt = it->second;
        int status = cpt.get<int>("status");
        if (status == 0)
			return true;
	}
	if (!m_log.empty())
	{
		ofstream fout(m_log, ofstream::app);
		time_t ttp = chrono::system_clock::to_time_t(chrono::system_clock::now());
		fout << "****" << std::ctime(&ttp) << "****" << endl;
		fout << "Order " << id << " for " << coin << " is executed" << endl;
	}
    return false;
}

void WexTradeApi::cancelCurrentOrders()
{
    Log l("WexTradeApi::cancelCurrentOrders()");
    std::vector<long long> orders = getCurrentOrders();
    for(auto id: orders)
        deleteOrder(id);
}

std::vector<long long> WexTradeApi::getCurrentOrders()
{
    Log l("WexTradeApi::getCurrentOrders()");
    std::map<std::string, std::string> params;
    params["method"] = "ActiveOrders";
    std::istringstream is(call(params));
    ptree pt;
    read_json(is, pt);
    std::string err = pt.get("error", "");
    if (err.size())
    {
        if(err == "no orders")
            return std::vector<long long>();
        Log::write("throw");
        throw std::runtime_error(err);
    }
    ptree result = pt.get_child("return");
    std::vector<long long> res;
    for (auto it: result)
    {
        std::string name = it.first;
        res.push_back(boost::lexical_cast<long long>(name));
    }
    return res;
}

std::vector<long long> WexTradeApi::getCurrentOrders(const string &coin)
{
    Log l(boost::str(boost::format("WexTradeApi::getCurrentOrders(%s)") %
                            coin.c_str()));
    std::map<std::string, std::string> params;
    params["method"] = "returnOpenOrders";
    params["pair"] = coin + "_btc";
    if(m_params[coin].reverted)
        params["pair"] = "btc_" + coin;
    std::istringstream is(call(params));
    ptree pt;
    read_json(is, pt);
    std::string err = pt.get("error", "");
    if (err.size())
    {
        Log::write("throw");
        throw std::runtime_error(err);
    }
    ptree result = pt.get_child("return");
    std::vector<long long> res;
    for (auto it: result)
    {
        std::string name = it.first;
        res.push_back(boost::lexical_cast<long long>(name));
    }
    return res;
}

void WexTradeApi::deleteOrder(long long id)
{
    Log l(boost::str(boost::format("WexTradeApi::deleteOrder(%d)") % id));
	std::map<std::string, std::string> params;
    params["method"] = "CancelOrder";
    params["order_id"] = boost::lexical_cast<std::string>(id);
    std::istringstream is(call(params));
	ptree pt;
	read_json(is, pt);
	std::string err = pt.get("error", "");
	if (!m_log.empty())
	{
		ofstream fout(m_log, ofstream::app);
		time_t ttp = chrono::system_clock::to_time_t(chrono::system_clock::now());
		fout << "****" << std::ctime(&ttp) << "****" << endl;
        fout << "Delete order " << id << endl;
		if (err.size())
			fout << "Error: " << err << endl;
	}
	if (err.size())
	{
		Log::write("throw");
		throw std::runtime_error(err);
	}
}

double WexTradeApi::balance(const std::string& coin)
{
    Log l(boost::str(boost::format("WexTradeApi::balance(%s)") % coin.c_str()));
	if (m_balances.empty())
		readBalances();
	auto it = m_balances.find(coin);
	if (it == m_balances.end())
	{
		Log::write("throw");
		throw runtime_error("Invalid coin");
	}
	return it->second;
}

TradeApi::CoinInfo WexTradeApi::info(const std::string& coin)
{
    Log l(boost::str(boost::format("WexTradeApi::info(%s)") % coin.c_str()));
	if (m_tickers.empty())
		readTickers();
	auto it = m_tickers.find(coin);
	if (it == m_tickers.end())
	{
		Log::write("throw");
		throw runtime_error("Invalid coin");
	}
	return it->second;
}

static void
load_root_certificates(ssl::context& ctx)
{
	std::string const cert =
		/*  This is the DigiCert root certificate.

		CN = DigiCert High Assurance EV Root CA
		OU = www.digicert.com
		O = DigiCert Inc
		C = US

		Valid to: Sunday, ?November ?9, ?2031 5:00:00 PM

		Thumbprint(sha1):
		5f b7 ee 06 33 e2 59 db ad 0c 4c 9a e6 d3 8f 1a 61 c7 dc 25
		*/
		"-----BEGIN CERTIFICATE-----\n"
		"MIIDxTCCAq2gAwIBAgIQAqxcJmoLQJuPC3nyrkYldzANBgkqhkiG9w0BAQUFADBs\n"
		"MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
		"d3cuZGlnaWNlcnQuY29tMSswKQYDVQQDEyJEaWdpQ2VydCBIaWdoIEFzc3VyYW5j\n"
		"ZSBFViBSb290IENBMB4XDTA2MTExMDAwMDAwMFoXDTMxMTExMDAwMDAwMFowbDEL\n"
		"MAkGA1UEBhMCVVMxFTATBgNVBAoTDERpZ2lDZXJ0IEluYzEZMBcGA1UECxMQd3d3\n"
		"LmRpZ2ljZXJ0LmNvbTErMCkGA1UEAxMiRGlnaUNlcnQgSGlnaCBBc3N1cmFuY2Ug\n"
		"RVYgUm9vdCBDQTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAMbM5XPm\n"
		"+9S75S0tMqbf5YE/yc0lSbZxKsPVlDRnogocsF9ppkCxxLeyj9CYpKlBWTrT3JTW\n"
		"PNt0OKRKzE0lgvdKpVMSOO7zSW1xkX5jtqumX8OkhPhPYlG++MXs2ziS4wblCJEM\n"
		"xChBVfvLWokVfnHoNb9Ncgk9vjo4UFt3MRuNs8ckRZqnrG0AFFoEt7oT61EKmEFB\n"
		"Ik5lYYeBQVCmeVyJ3hlKV9Uu5l0cUyx+mM0aBhakaHPQNAQTXKFx01p8VdteZOE3\n"
		"hzBWBOURtCmAEvF5OYiiAhF8J2a3iLd48soKqDirCmTCv2ZdlYTBoSUeh10aUAsg\n"
		"EsxBu24LUTi4S8sCAwEAAaNjMGEwDgYDVR0PAQH/BAQDAgGGMA8GA1UdEwEB/wQF\n"
		"MAMBAf8wHQYDVR0OBBYEFLE+w2kD+L9HAdSYJhoIAu9jZCvDMB8GA1UdIwQYMBaA\n"
		"FLE+w2kD+L9HAdSYJhoIAu9jZCvDMA0GCSqGSIb3DQEBBQUAA4IBAQAcGgaX3Nec\n"
		"nzyIZgYIVyHbIUf4KmeqvxgydkAQV8GK83rZEWWONfqe/EW1ntlMMUu4kehDLI6z\n"
		"eM7b41N5cdblIZQB2lWHmiRk9opmzN6cN82oNLFpmyPInngiK3BD41VHMWEZ71jF\n"
		"hS9OMPagMRYjyOfiZRYzy78aG6A9+MpeizGLYAiJLQwGXFK3xPkKmNEVX58Svnw2\n"
		"Yzi9RKR/5CYrCsSXaQ3pjOLAEFe4yHYSkVXySGnYvCoCWw9E1CAx2/S6cCZdkGCe\n"
		"vEsXCS+0yx5DaMkHJ8HSXPfqIbloEpw8nL+e/IBcm2PN7EeqJSdnoDfzAIJ9VNep\n"
		"+OkuE6N36B9K\n"
		"-----END CERTIFICATE-----\n"
		/*  This is the GeoTrust root certificate.

		CN = GeoTrust Global CA
		O = GeoTrust Inc.
		C = US
		Valid to: Friday, ‎May ‎20, ‎2022 9:00:00 PM

		Thumbprint(sha1):
		‎de 28 f4 a4 ff e5 b9 2f a3 c5 03 d1 a3 49 a7 f9 96 2a 82 12
		*/
		"-----BEGIN CERTIFICATE-----\n"
		"MIIDxTCCAq2gAwIBAgIQAqxcJmoLQJuPC3nyrkYldzANBgkqhkiG9w0BAQUFADBs\n"
		"MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
		"d3cuZGlnaWNlcnQuY29tMSswKQYDVQQDEyJEaWdpQ2VydCBIaWdoIEFzc3VyYW5j\n"
		"ZSBFViBSb290IENBMB4XDTA2MTExMDAwMDAwMFoXDTMxMTExMDAwMDAwMFowbDEL\n"
		"MAkGA1UEBhMCVVMxFTATBgNVBAoTDERpZ2lDZXJ0IEluYzEZMBcGA1UECxMQd3d3\n"
		"LmRpZ2ljZXJ0LmNvbTErMCkGA1UEAxMiRGlnaUNlcnQgSGlnaCBBc3N1cmFuY2Ug\n"
		"RVYgUm9vdCBDQTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAMbM5XPm\n"
		"+9S75S0tMqbf5YE/yc0lSbZxKsPVlDRnogocsF9ppkCxxLeyj9CYpKlBWTrT3JTW\n"
		"PNt0OKRKzE0lgvdKpVMSOO7zSW1xkX5jtqumX8OkhPhPYlG++MXs2ziS4wblCJEM\n"
		"xChBVfvLWokVfnHoNb9Ncgk9vjo4UFt3MRuNs8ckRZqnrG0AFFoEt7oT61EKmEFB\n"
		"Ik5lYYeBQVCmeVyJ3hlKV9Uu5l0cUyx+mM0aBhakaHPQNAQTXKFx01p8VdteZOE3\n"
		"hzBWBOURtCmAEvF5OYiiAhF8J2a3iLd48soKqDirCmTCv2ZdlYTBoSUeh10aUAsg\n"
		"EsxBu24LUTi4S8sCAwEAAaNjMGEwDgYDVR0PAQH/BAQDAgGGMA8GA1UdEwEB/wQF\n"
		"MAMBAf8wHQYDVR0OBBYEFLE+w2kD+L9HAdSYJhoIAu9jZCvDMB8GA1UdIwQYMBaA\n"
		"FLE+w2kD+L9HAdSYJhoIAu9jZCvDMA0GCSqGSIb3DQEBBQUAA4IBAQAcGgaX3Nec\n"
		"nzyIZgYIVyHbIUf4KmeqvxgydkAQV8GK83rZEWWONfqe/EW1ntlMMUu4kehDLI6z\n"
		"eM7b41N5cdblIZQB2lWHmiRk9opmzN6cN82oNLFpmyPInngiK3BD41VHMWEZ71jF\n"
		"hS9OMPagMRYjyOfiZRYzy78aG6A9+MpeizGLYAiJLQwGXFK3xPkKmNEVX58Svnw2\n"
		"Yzi9RKR/5CYrCsSXaQ3pjOLAEFe4yHYSkVXySGnYvCoCWw9E1CAx2/S6cCZdkGCe\n"
		"vEsXCS+0yx5DaMkHJ8HSXPfqIbloEpw8nL+e/IBcm2PN7EeqJSdnoDfzAIJ9VNep\n"
		"+OkuE6N36B9K\n"
		"-----END CERTIFICATE-----\n"
		;

	boost::system::error_code ec;
	ctx.add_certificate_authority(
		boost::asio::buffer(cert.data(), cert.size()), ec);
	if (ec)
		throw boost::system::system_error{ ec };
}

void WexTradeApi::readTickers()
{
    Log l("WexTradeApi::readTickers()");

    // get pair list
	std::stringstream is; 
    is << public_get("/api/3/info");
	ptree pt;
	read_json(is, pt);
    ptree pair_tree = pt.get_child("pairs");
    std::vector<std::string> pairs;
    for (ptree::iterator it = pair_tree.begin(); it != pair_tree.end(); ++it)
	{
        std::string pair_name = it->first;
        if (pair_name.find("btc") == std::string::npos)
			continue;
        PairParams params;
        ptree param_data = it->second;
        std::string coin;
        if(pair_name.substr(0, 4) != "btc_")
        {
            coin = pair_name.substr(0, pair_name.find("_"));
            params.reverted = false;
        }
        else
        {
            coin = pair_name.substr(pair_name.find("_") + 1);
            params.reverted = true;
        }
        params.decimal_places = param_data.get<unsigned>("decimal_places");
        params.fee = param_data.get<double>("fee");
        params.max = param_data.get<double>("max_price");
        params.min = param_data.get<double>("min_price");
        params.min_amount = param_data.get<double>("min_amount");
        m_params[coin] = params;
        pairs.push_back(pair_name);
	}
    // get tickers for this pairs
    std::string path = boost::algorithm::join(pairs, "-");
    is << public_get("/api/3/ticker/" + path);
    read_json(is, pt);
    for(auto it: pt)
    {
        std::string pair_name = it.first;
        std::string coin;
        CoinInfo i;
        if(pair_name.substr(0, 4) != "btc_")
        {
            coin = pair_name.substr(0, pair_name.find("_"));
            i.coin = coin;
            i.buyPrice = it.second.get<double>("buy");
            i.sellPrice = it.second.get<double>("sell");
            i.lastPrice = it.second.get<double>("last");
        }
        else
        {
            coin = pair_name.substr(pair_name.find("_") + 1);
            i.coin = coin;
            i.buyPrice = 1.0 / it.second.get<double>("buy");
            i.sellPrice = 1.0 / it.second.get<double>("sell");
            i.lastPrice = 1.0 / it.second.get<double>("last");
        }
        m_tickers[coin] = i;
    }
}

bool is_token(const std::string& name)
{
    if(name.length() < 2)
        return false;
    size_t pos = name.length() - 2;
    return (name.substr(pos, 2) == "et");
}

void WexTradeApi::readBalances()
{
    Log l("WexTradeApi::readBalances()");
	m_balances.clear();
	std::map<std::string, std::string> params;
    params["method"] = "getInfo";
	std::istringstream is(call(params));
	ptree pt;
	read_json(is, pt);
	std::string err = pt.get("error", "");
	if (err.size())
	{
		Log::write("throw");
		throw std::runtime_error(err);
	}
    ptree funds = pt.get_child("return").get_child("funds");
    for (ptree::iterator it = funds.begin(); it != funds.end(); ++it)
	{
        if(is_token(it->first))
            continue;
        double balance = boost::lexical_cast<double>(it->second.data());
        if(balance > 0.001)
            m_balances[it->first] = balance;
    }
}

string WexTradeApi::public_get(const string &target)
{
    // connection params
    std::string host("wex.nz");
    std::string port("443");

    // The io_service is required for all I/O
    boost::asio::io_service ios;

    // The SSL context is required, and holds certificates
    ssl::context ctx{ ssl::context::sslv23_client };

    // This holds the root certificate used for verification
    load_root_certificates(ctx);

    // These objects perform our I/O
    tcp::resolver resolver{ ios };
    ssl::stream<tcp::socket> stream{ ios, ctx };

    // Look up the domain name
    auto const lookup = resolver.resolve({ host, port });

    // Make the connection on the IP address we get from a lookup
    boost::asio::connect(stream.next_layer(), lookup);

    // Perform the SSL handshake
    stream.handshake(ssl::stream_base::client);

    // Set up an HTTP GET request message
    http::request<http::string_body> req{ http::verb::get, target, 11 };
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

    // Send the HTTP request to the remote host
    http::write(stream, req);

    // This buffer is used for reading and must be persisted
    boost::beast::flat_buffer buffer;

    // Declare a container to hold the response
    http::response<http::string_body> res;

    // Receive the HTTP response
    http::read(stream, buffer, res);

    // Write the message to string stream to read in parser
    std::stringstream is;
    is << res.body();

    // Gracefully close the stream
    boost::system::error_code ec;
    stream.shutdown(ec);

    return is.str();
}

std::string WexTradeApi::call(const std::map<std::string, std::string>& params)
{
    Log l("WexTradeApi::call");
    // connection params
    std::string host("wex.nz");
    std::string port("443");
    std::string target("/tapi");

    std::string postData = postBody(params);
	std::string sign = signBody(postData);

    // The io_service is required for all I/O
    boost::asio::io_service ios;

    // The SSL context is required, and holds certificates
    ssl::context ctx{ ssl::context::sslv23_client };

    // This holds the root certificate used for verification
    load_root_certificates(ctx);

    // These objects perform our I/O
    tcp::resolver resolver{ ios };
    ssl::stream<tcp::socket> stream{ ios, ctx };

    // Look up the domain name
    auto const lookup = resolver.resolve({ host, port });

    // Make the connection on the IP address we get from a lookup
    boost::asio::connect(stream.next_layer(), lookup);

    // Perform the SSL handshake
    stream.handshake(ssl::stream_base::client);

    // Set up an HTTP POST request message
    http::request<http::string_body> req;
    req.method(http::verb::post);
    req.target(target);
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(http::field::content_type,
            "application/x-www-form-urlencoded");
    req.set("Key", m_key);
    req.set("Sign", sign);
    req.body() = postData;
    req.prepare_payload();

    // Send the HTTP request to the remote host
    http::write(stream, req);

    // This buffer is used for reading and must be persisted
    boost::beast::flat_buffer buffer;

    // Declare a container to hold the response
    http::response<http::string_body> res;

    // Receive the HTTP response
    http::read(stream, buffer, res);

    // Write the message to output stream
    std::string reply(res.body());

    // Gracefully close the stream
    boost::system::error_code ec;
    stream.shutdown(ec);
/*    if (ec == boost::asio::error::eof)
    {
        // Rationale:
        // http://stackoverflow.com/questions/25587403/boost-asio-ssl-async-shutdown-always-finishes-with-an-error
        ec.assign(0, ec.category());
    }
    else if (ec)
        throw boost::system::system_error{ ec };*/
    // If we get here then the connection is closed gracefully
    return reply;
}

std::string WexTradeApi::postBody(const std::map<std::string, std::string>& params)
{
    Log l("WexTradeApi::postBody");
	std::string res = (boost::format("nonce=%d") % m_nonce++).str();
	for (auto param : params)
		res += "&" + param.first + "=" + param.second;
    Log::write(boost::str(boost::format("Result: %s") % res.c_str()));
	return res;
}

std::string WexTradeApi::signBody(const std::string& body)
{
    Log l("WexTradeApi::signBody");
    Log::write(boost::str(boost::format("Body: %s") % body.c_str()));
	unsigned char* digest = HMAC(EVP_sha512(),
		m_secret.c_str(), m_secret.length(),
		(unsigned char*)body.c_str(), body.length(), NULL, NULL);
	char mdString[129];
	for (int i = 0; i < 64; i++)
		sprintf(&mdString[i * 2], "%02x", (unsigned int)digest[i]);
	mdString[128] = 0;
    Log::write(boost::str(boost::format("Sign: %s") % mdString));
	return mdString;
}

map<std::string, double> WexTradeApi::nonZeroBalancesInBTC()
{
    Log l("WexTradeApi::nonZeroBalancesInBTC()");
	if (m_balances.empty())
		readBalances();
	if (m_tickers.empty())
		readTickers();
	map<std::string, double> balances;
	for (auto b : m_balances)
	{
		if (b.second == 0.0)
			continue;
        if (b.first == "btc")
		{
			balances[b.first] = b.second;
            Log::write(boost::str(boost::format("%s:%f") % b.first.c_str() % b.second));
		}
		else
		{
			TradeApi::CoinInfo ci = m_tickers[b.first];
			double amount = b.second * (ci.buyPrice + ci.sellPrice) / 2;
			balances[b.first] = amount;
            Log::write(boost::str(boost::format("%s:%f") % b.first.c_str() % amount));
		}
	}
	return balances;
}

std::map<std::string, double> WexTradeApi::nonZeroBalances()
{
    Log l("WexTradeApi::nonZeroBalances()");
	if (m_balances.empty())
		readBalances();
	if (m_tickers.empty())
		readTickers();
	map<std::string, double> balances;
	for (auto b : m_balances)
	{
		if (b.second == 0.0)
			continue;
		balances[b.first] = b.second;
        Log::write(boost::str(boost::format("%s:%f") % b.first.c_str() % b.second));
	}
	return balances;
}

