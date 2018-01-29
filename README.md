This utility allows you to automatically rebalance your [Wex.nz](http://www.wex.nz) cryptocoin portfolio.

#USAGE

You can read about portfolio rebalance [here](http://www.bogleheads.org/wiki/Rebalancing)

Example: 
If you want to have equal parts of BTC and DSH and twice bigger part of ZEC (25% of BTC, 25% of DSH and 50% of ZEC), for example,  you can call periodically 
**wex_manager -c btc -p 1 -c zec -p 2 -c dsh -p 1 -k your_wex_api_key -s your_wex_api_secret -t 10 --timeout 60**
with Task Scheduler on Windows or cron on Linux or just manually.

You can ran 
**wex_manager --help**
to read about command line options

#BUILD
Project depends on Boost, Beast (part of Boost starting from Boost 1.66) and OpenSSL. After installing this libs use CMake to build it with you favorite compiler.
