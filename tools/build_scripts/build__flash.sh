#!/bin/bash
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

echo "-----------------------------------------------------------------------------------------------------------------------------------"
echo "Removing old library dependencies for ${env_name} ..."
echo "-----------------------------------------------------------------------------------------------------------------------------------"
rm -rf ".pio/libdeps/${env_name}/W25N"*
rm -rf ".pio/libdeps/${env_name}/MAX2871"*
pio run -t clean && pio run "$@"
