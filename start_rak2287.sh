#!/usr/bin/env bash 

# Defaults to TTN server v2
TTN_STACK_VERSION=${TTN_STACK_VERSION:-2}
if [ $TTN_STACK_VERSION -eq 2 ]; then
	TC_URI=${TC_URI:-"wss://lns.eu.thethings.network:443"} 
	TC_TRUST=${TC_TRUST:-$(curl --silent "https://letsencrypt.org/certs/trustid-x3-root.pem.txt"))}
elif [ $TTN_STACK_VERSION -eq 3 ]; then
	TC_URI=${TC_URI:-"wss://eu1.cloud.thethings.network:8887"} 
	TC_TRUST=${TC_TRUST:-$(curl --silent "https://letsencrypt.org/certs/{trustid-x3-root.pem.txt,isrgrootx1.pem}"))}
else
    echo -e "\033[91mERROR: Wrong TTN_STACK_VERSION value, should be either 2 o 3.\033[0m"
	balena-idle
fi

# Check configuration
if [ $TC_URI = "" ] || [ $TC_TRUST == "" ]; then
    echo -e "\033[91mERROR: Missing configuration, define either TTN_STACK_VERSION or TC_URI and TC_TRUST.\033[0m"
	balena-idle
fi

# declare map of hardware pins to GPIO on Raspberry Pi
declare -a pinToGPIO
pinToGPIO=( -1 -1 -1 2 -1 3 -1 4 14 -1 15 17 18 27 -1 22 23 -1 24 10 -1 9 25 11 8 -1 7 0 1 5 -1 6 12 13 -1 19 16 26 20 -1 21)
GW_RESET_PIN=${GW_RESET_PIN:-11}
GW_RESET_GPIO=${GW_RESET_GPIO:-${pinToGPIO[$GW_RESET_PIN]}}

cd examples/corecell

# Setup TC files from environment
echo "$TC_URI" > ./lns-ttn/tc.uri
echo "$TC_TRUST" > ./lns-ttn/tc.trust
if [ ! -z ${TC_KEY} ]; then
	echo "Authorization: Bearer $TC_KEY" | perl -p -e 's/\r\n|\n|\r/\r\n/g'  > ./lns-ttn/tc.key
fi

# Set other environment variables
export GW_RESET_GPIO=$GW_RESET_GPIO

./start-station.sh -l ./lns-ttn

balena-idle
