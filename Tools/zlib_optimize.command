#!/bin/bash

# zlib_optimize.command
# Usage: ./zlib_optimize.command
#
# Created by Rodion Shingarev on 17/05/15.
#

MyPath=$(dirname "$BASH_SOURCE")
pushd "$MyPath/../" &>/dev/null

find ./Resources -name 'Platform*.xml' | while read file
do
	echo "Optimizing" $file
	#perl zlib.pl deflate "$file" > "$file.zlib"
	/usr/libexec/PlistBuddy -c "Delete :CommonPeripheralDSP" $file 2>/dev/null || true
	/usr/libexec/PlistBuddy -c "Add CommonPeripheralDSP array" $file || { echo "Failed to add CommonPeripheralDSP array to $file"; continue; }
	/usr/libexec/PlistBuddy -c "Add CommonPeripheralDSP:0 dict" $file || { echo "Failed to add dict to $file"; continue; }
	/usr/libexec/PlistBuddy -c "Add CommonPeripheralDSP:1 dict" $file || { echo "Failed to add dict to $file"; continue; }

	/usr/libexec/PlistBuddy -c "Add CommonPeripheralDSP:0:DeviceID integer" $file || { echo "Failed to add DeviceID to $file"; continue; }
	/usr/libexec/PlistBuddy -c "Add CommonPeripheralDSP:0:DeviceType string" $file || { echo "Failed to add DeviceType to $file"; continue; }
	/usr/libexec/PlistBuddy -c "Add CommonPeripheralDSP:1:DeviceID integer" $file || { echo "Failed to add DeviceID to $file"; continue; }
	/usr/libexec/PlistBuddy -c "Add CommonPeripheralDSP:1:DeviceType string" $file || { echo "Failed to add DeviceType to $file"; continue; }

	/usr/libexec/PlistBuddy -c "Set CommonPeripheralDSP:0:DeviceType Headphone" $file || { echo "Failed to set DeviceType to $file"; continue; }
	/usr/libexec/PlistBuddy -c "Set CommonPeripheralDSP:1:DeviceType Microphone" $file || { echo "Failed to set DeviceType to $file"; continue; }
done

popd &>/dev/null
