#!/bin/bash

command="ls -go --full-time"
env="technician"
# W25N flash library project
W25n_pio_deps=~/projects/embedded-applications/saTech/.pio/libdeps/$env/W25N_Flash_library/src
W25n_library=~/projects/embedded-device-drivers/W25N_Flash_library/src
# MAX2871 PLL Synthesizer library project
MAX_pio_deps=~/projects/embedded-applications/saTech/.pio/libdeps/$env/MAX2871_PLL/src
MAX_library=~/projects/embedded-device-drivers/MAX2871_library_dev/src
# File changed status constants
W25n_files_changed=1
W25n_files_same=0
MAX_files_changed=1
MAX_files_same=0

# Verify command line parameters
if [ $# -eq 0 ]; then
    echo "Usage: $0 <pio run arguments>"
    echo "Example: $0 -e technician -t upload --upload-port /dev/ttyUSB0"
    exit 1
fi

# Extract the env name passed via -e/--environment
env_name=""
args=("$@")
for i in "${!args[@]}"; do
    case "${args[$i]}" in
        -e|--environment)
            env_name="${args[$((i+1))]}"
            break
            ;;
        -e*)
            env_name="${args[$i]#-e}"
            break
            ;;
        --environment=*)
            env_name="${args[$i]#--environment=}"
            break
            ;;
    esac
done

if [ -z "$env_name" ]; then
    echo "Error: could not determine env name from arguments (expected -e <env>)"
    exit 1
fi

# Grab a hash of the W25N Flash library source files
pio_w25n_hash=$($command "$W25n_pio_deps" | md5sum | cut -d " " -f 1)
lib_w25n_hash=$($command "$W25n_library" | md5sum | cut -d " " -f 1)

# Grab a hash of the MAX2871 PLL library source files
pio_MAX_hash=$(ls -go --full-time "$MAX_pio_deps" | md5sum | cut -d " " -f 1)
lib_MAX_hash=$(ls -go --full-time "$MAX_library" | md5sum | cut -d " " -f 1)


echo "--------------------------------------------------------------------------------------------"

# Check if the W25N Flash library source files were changed
if [[ "$pio_w25n_hash" != "$lib_w25n_hash" ]]
then
    w25n_result=$W25n_files_changed
else
    w25n_result=$W25n_files_same
fi

# Report and delete stale W25N library dependencies
if [[ "$w25n_result" == "$W25n_files_changed" ]]
then
    echo "Removing old W25N Flash library dependencies for ${env_name} ..."
    rm -rf ".pio/libdeps/${env_name}/W25N"*
else
    echo "W25N Flash library is unchanged in the '$env' environment"
fi


# Check if the MAX2871 PLL library source files were changed
if [[ "$pio_MAX_hash" != "$lib_MAX_hash" ]]
then
    max_result=$MAX_files_changed
else
    max_result=$MAX_files_same
fi

# Report and delete stale MAX2871 library dependencies
if [[ "$max_result" == "$MAX_files_changed" ]]
then
    echo "Removing old MAX2871 library dependencies for ${env_name} ..."
    rm -rf ".pio/libdeps/${env_name}/MAX2871"*
else
    echo "MAX2871 library is unchanged in the '$env' environment"
fi

echo "--------------------------------------------------------------------------------------------"

pio run -t clean && pio run "$@"

