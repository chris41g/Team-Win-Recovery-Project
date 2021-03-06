/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <linux/input.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

#include <string>
#include <utility>
#include <map>
#include <fstream>
#include <sstream>

#include "variables.h"
#include "data.hpp"

extern "C"
{
    #include "common.h"
    #include "data.h"
	#include "ddftw.h"
    #include "tw_reboot.h"
	#include "roots.h"

	int ensure_path_mounted(const char* path);
	int mount_current_storage(void);

    int get_battery_level(void);
    void get_device_id(void);

    extern char device_id[15];

    void gui_notifyVarChange(const char *name, const char* value);
}

#define FILE_VERSION    0x00010001

using namespace std;

map<string, DataManager::TStrIntPair>   DataManager::mValues;
map<string, string>                     DataManager::mConstValues;
string                                  DataManager::mBackingFile;
int                                     DataManager::mInitialized = 0;

int DataManager::ResetDefaults()
{
    mValues.clear();
    mConstValues.clear();
    SetDefaultValues();
    return 0;
}

int DataManager::LoadValues(const string filename)
{
    string str;

	if (!mInitialized)
        SetDefaultValues();

    // Save off the backing file for set operations
    mBackingFile = filename;

    // Read in the file, if possible
    FILE* in = fopen(filename.c_str(), "rb");
    if (!in)    return 0;

    int file_version;
    if (fread(&file_version, 1, sizeof(int), in) != sizeof(int))    goto error;
    if (file_version != FILE_VERSION)                               goto error;

    while (!feof(in))
    {
        string Name;
        string Value;
        unsigned short length;
        char array[512];

        if (fread(&length, 1, sizeof(unsigned short), in) != sizeof(unsigned short))    goto error;
        if (length >= 512)                                                              goto error;
        if (fread(array, 1, length, in) != length)                                      goto error;
        Name = array;

        if (fread(&length, 1, sizeof(unsigned short), in) != sizeof(unsigned short))    goto error;
        if (length >= 512)                                                              goto error;
        if (fread(array, 1, length, in) != length)                                      goto error;
        Value = array;

        map<string, TStrIntPair>::iterator pos;

        pos = mValues.find(Name);
        if (pos != mValues.end())
        {
            pos->second.first = Value;
            pos->second.second = 1;
        }
        else
            mValues.insert(TNameValuePair(Name, TStrIntPair(Value, 1)));
    }
    fclose(in);

	str = GetCurrentStoragePath();
	str += "/TWRP/BACKUPS/";
	str += device_id;
	SetValue(TW_BACKUPS_FOLDER_VAR, str, 0);

    return 0;

error:
    // File version mismatch. Use defaults.
    fclose(in);
	str = GetCurrentStoragePath();
	str += "/TWRP/BACKUPS/";
	str += device_id;
	SetValue(TW_BACKUPS_FOLDER_VAR, str, 0);
    return -1;
}

int DataManager::Flush()
{
    return SaveValues();
}

int DataManager::SaveValues()
{
    if (mBackingFile.empty())       return -1;

	string mount_path = GetSettingsStoragePath();
	ensure_path_mounted(mount_path.c_str());

	FILE* out = fopen(mBackingFile.c_str(), "wb");
    if (!out)                       return -1;

    int file_version = FILE_VERSION;
    fwrite(&file_version, 1, sizeof(int), out);

    map<string, TStrIntPair>::iterator iter;
    for (iter = mValues.begin(); iter != mValues.end(); ++iter)
    {
        // Save only the persisted data
        if (iter->second.second != 0)
        {
            unsigned short length = (unsigned short) iter->first.length() + 1;
            fwrite(&length, 1, sizeof(unsigned short), out);
            fwrite(iter->first.c_str(), 1, length, out);
            length = (unsigned short) iter->second.first.length() + 1;
            fwrite(&length, 1, sizeof(unsigned short), out);
            fwrite(iter->second.first.c_str(), 1, length, out);
        }
    }
    fclose(out);
    return 0;
}

int DataManager::GetValue(const string varName, string& value)
{
    string localStr = varName;

    if (!mInitialized)
        SetDefaultValues();

    // Strip off leading and trailing '%' if provided
    if (localStr.length() > 2 && localStr[0] == '%' && localStr[localStr.length()-1] == '%')
    {
        localStr.erase(0, 1);
        localStr.erase(localStr.length() - 1, 1);
    }

    // Handle magic values
    if (GetMagicValue(localStr, value) == 0)     return 0;

    map<string, string>::iterator constPos;
    constPos = mConstValues.find(localStr);
    if (constPos != mConstValues.end())
    {
        value = constPos->second;
        return 0;
    }

    map<string, TStrIntPair>::iterator pos;
    pos = mValues.find(localStr);
    if (pos == mValues.end())
        return -1;

    value = pos->second.first;
    return 0;
}

int DataManager::GetValue(const string varName, int& value)
{
    string data;

    if (GetValue(varName,data) != 0)
        return -1;

    value = atoi(data.c_str());
    return 0;
}

// This is a dangerous function. It will create the value if it doesn't exist so it has a valid c_str
string& DataManager::GetValueRef(const string varName)
{
    if (!mInitialized)
        SetDefaultValues();

    map<string, string>::iterator constPos;
    constPos = mConstValues.find(varName);
    if (constPos != mConstValues.end())
        return constPos->second;

    map<string, TStrIntPair>::iterator pos;
    pos = mValues.find(varName);
    if (pos == mValues.end())
        pos = (mValues.insert(TNameValuePair(varName, TStrIntPair("", 0)))).first;

    return pos->second.first;
}

// This function will return an empty string if the value doesn't exist
string DataManager::GetStrValue(const string varName)
{
    string retVal;

    GetValue(varName, retVal);
    return retVal;
}

// This function will return 0 if the value doesn't exist
int DataManager::GetIntValue(const string varName)
{
    string retVal;

    GetValue(varName, retVal);
    return atoi(retVal.c_str());
}

int DataManager::SetValue(const string varName, string value, int persist /* = 0 */)
{
    if (!mInitialized)
        SetDefaultValues();

    // Don't allow empty values or numerical starting values
    if (varName.empty() || (varName[0] >= '0' && varName[0] <= '9'))
        return -1;

    map<string, string>::iterator constChk;
    constChk = mConstValues.find(varName);
    if (constChk != mConstValues.end())
        return -1;

    map<string, TStrIntPair>::iterator pos;
    pos = mValues.find(varName);
    if (pos == mValues.end())
        pos = (mValues.insert(TNameValuePair(varName, TStrIntPair(value, persist)))).first;
    else
        pos->second.first = value;

    if (pos->second.second != 0)
        SaveValues();

    gui_notifyVarChange(varName.c_str(), value.c_str());
    return 0;
}

int DataManager::SetValue(const string varName, int value, int persist /* = 0 */)
{
	ostringstream valStr;
    valStr << value;
	if (varName == "tw_use_external_storage") {
		string str;

		if (GetIntValue(TW_HAS_DUAL_STORAGE) == 1) {
			if (value == 0) {
				str = GetStrValue(TW_INTERNAL_PATH);
				SetValue(TW_STORAGE_FREE_SIZE, (int)((sdcint.sze - sdcint.used) / 1048576LLU));
			} else {
				str = GetStrValue(TW_EXTERNAL_PATH);
				SetValue(TW_STORAGE_FREE_SIZE, (int)((sdcext.sze - sdcext.used) / 1048576LLU));
			}
		} else if (GetIntValue(TW_HAS_INTERNAL) == 1)
			str = GetStrValue(TW_INTERNAL_PATH);
		else
			str = GetStrValue(TW_EXTERNAL_PATH);

		str += "/TWRP/BACKUPS/";
		str += device_id;

		SetValue(TW_BACKUPS_FOLDER_VAR, str);
	}
    return SetValue(varName, valStr.str(), persist);;
}

int DataManager::SetValue(const string varName, float value, int persist /* = 0 */)
{
	ostringstream valStr;
    valStr << value;
    return SetValue(varName, valStr.str(), persist);;
}

void DataManager::DumpValues()
{
    map<string, TStrIntPair>::iterator iter;
    ui_print("Data Manager dump - Values with leading X are persisted.\n");
    for (iter = mValues.begin(); iter != mValues.end(); ++iter)
    {
        ui_print("%c %s=%s\n", iter->second.second ? 'X' : ' ', iter->first.c_str(), iter->second.first.c_str());
    }
}

void DataManager::SetDefaultValues()
{
    string str, path;

    get_device_id();

    mInitialized = 1;

    mConstValues.insert(make_pair("true", "1"));
    mConstValues.insert(make_pair("false", "0"));

    mConstValues.insert(make_pair(TW_VERSION_VAR, TW_VERSION_STR));

#ifdef BOARD_HAS_NO_REAL_SDCARD
    mConstValues.insert(make_pair(TW_ALLOW_PARTITION_SDCARD, "0"));
#else
    mConstValues.insert(make_pair(TW_ALLOW_PARTITION_SDCARD, "1"));
#endif

#ifdef TW_INCLUDE_DUMLOCK
	mConstValues.insert(make_pair(TW_SHOW_DUMLOCK, "1"));
#else
	mConstValues.insert(make_pair(TW_SHOW_DUMLOCK, "0"));
#endif

#ifdef TW_INTERNAL_STORAGE_PATH
	LOGI("Internal path defined: '%s'\n", EXPAND(TW_INTERNAL_STORAGE_PATH));
	mValues.insert(make_pair(TW_USE_EXTERNAL_STORAGE, make_pair("0", 1)));
	mConstValues.insert(make_pair(TW_HAS_INTERNAL, "1"));
	mConstValues.insert(make_pair(TW_INTERNAL_PATH, EXPAND(TW_INTERNAL_STORAGE_PATH)));
	mConstValues.insert(make_pair(TW_INTERNAL_LABEL, EXPAND(TW_INTERNAL_STORAGE_MOUNT_POINT)));
	mValues.insert(make_pair(TW_ZIP_INTERNAL_VAR, make_pair(EXPAND(TW_INTERNAL_STORAGE_PATH), 1)));
	path.clear();
	path = "/";
	path += EXPAND(TW_INTERNAL_STORAGE_MOUNT_POINT);
	mConstValues.insert(make_pair(TW_INTERNAL_MOUNT, path));
	#ifdef TW_EXTERNAL_STORAGE_PATH
		LOGI("External path defined: '%s'\n", EXPAND(TW_EXTERNAL_STORAGE_PATH));
		// Device has dual storage
		mConstValues.insert(make_pair(TW_HAS_DUAL_STORAGE, "1"));
		mConstValues.insert(make_pair(TW_HAS_EXTERNAL, "1"));
		mConstValues.insert(make_pair(TW_EXTERNAL_PATH, EXPAND(TW_EXTERNAL_STORAGE_PATH)));
		mConstValues.insert(make_pair(TW_EXTERNAL_LABEL, EXPAND(TW_EXTERNAL_STORAGE_MOUNT_POINT)));
		mValues.insert(make_pair(TW_ZIP_EXTERNAL_VAR, make_pair(EXPAND(TW_EXTERNAL_STORAGE_PATH), 1)));
		path.clear();
		path = "/";
		path += EXPAND(TW_EXTERNAL_STORAGE_MOUNT_POINT);
		mConstValues.insert(make_pair(TW_EXTERNAL_MOUNT, path));
	#else
		LOGI("Just has internal storage.\n");
		// Just has internal storage
		mConstValues.insert(make_pair(TW_HAS_DUAL_STORAGE, "0"));
		mConstValues.insert(make_pair(TW_HAS_EXTERNAL, "0"));
		mConstValues.insert(make_pair(TW_EXTERNAL_PATH, "0"));
		mConstValues.insert(make_pair(TW_EXTERNAL_MOUNT, "0"));
		mConstValues.insert(make_pair(TW_EXTERNAL_LABEL, "0"));
	#endif
#else
	#ifdef RECOVERY_SDCARD_ON_DATA
		#ifdef TW_EXTERNAL_STORAGE_PATH
			LOGI("Has /data/media + external storage in '%s'\n", EXPAND(TW_EXTERNAL_STORAGE_PATH));
			// Device has /data/media + external storage
			mConstValues.insert(make_pair(TW_HAS_DUAL_STORAGE, "1"));
		#else
			LOGI("Single storage only.\n");
			// Device just has external storage
			mConstValues.insert(make_pair(TW_HAS_DUAL_STORAGE, "0"));
		#endif
	#else
		LOGI("Single storage only.\n");
		// Device just has external storage
		mConstValues.insert(make_pair(TW_HAS_DUAL_STORAGE, "0"));
	#endif
	#ifdef RECOVERY_SDCARD_ON_DATA
		LOGI("Device has /data/media defined.\n");
		// Device has /data/media
		mConstValues.insert(make_pair(TW_USE_EXTERNAL_STORAGE, "0"));
		mConstValues.insert(make_pair(TW_HAS_INTERNAL, "1"));
		mConstValues.insert(make_pair(TW_INTERNAL_PATH, "/data/media"));
		mConstValues.insert(make_pair(TW_INTERNAL_MOUNT, "/data"));
		mConstValues.insert(make_pair(TW_INTERNAL_LABEL, "data"));
		#ifdef TW_EXTERNAL_STORAGE_PATH
			mValues.insert(make_pair(TW_ZIP_INTERNAL_VAR, make_pair("/data/media", 1)));
		#else
			mValues.insert(make_pair(TW_ZIP_INTERNAL_VAR, make_pair("/sdcard", 1)));
		#endif
	#else
		LOGI("No internal storage defined.\n");
		// Device has no internal storage
		mConstValues.insert(make_pair(TW_USE_EXTERNAL_STORAGE, "1"));
		mConstValues.insert(make_pair(TW_HAS_INTERNAL, "0"));
		mConstValues.insert(make_pair(TW_INTERNAL_PATH, "0"));
		mConstValues.insert(make_pair(TW_INTERNAL_MOUNT, "0"));
		mConstValues.insert(make_pair(TW_INTERNAL_LABEL, "0"));
	#endif
	#ifdef TW_EXTERNAL_STORAGE_PATH
		LOGI("Only external path defined: '%s'\n", EXPAND(TW_EXTERNAL_STORAGE_PATH));
		// External has custom definition
		mConstValues.insert(make_pair(TW_HAS_EXTERNAL, "1"));
		mConstValues.insert(make_pair(TW_EXTERNAL_PATH, EXPAND(TW_EXTERNAL_STORAGE_PATH)));
		mConstValues.insert(make_pair(TW_EXTERNAL_LABEL, EXPAND(TW_EXTERNAL_STORAGE_MOUNT_POINT)));
		mValues.insert(make_pair(TW_ZIP_EXTERNAL_VAR, make_pair(EXPAND(TW_EXTERNAL_STORAGE_PATH), 1)));
		path.clear();
		path = "/";
		path += EXPAND(TW_EXTERNAL_STORAGE_MOUNT_POINT);
		mConstValues.insert(make_pair(TW_EXTERNAL_MOUNT, path));
	#else
		#ifndef RECOVERY_SDCARD_ON_DATA
			LOGI("No storage defined, defaulting to /sdcard.\n");
			// Standard external definition
			mConstValues.insert(make_pair(TW_HAS_EXTERNAL, "1"));
			mConstValues.insert(make_pair(TW_EXTERNAL_PATH, "/sdcard"));
			mConstValues.insert(make_pair(TW_EXTERNAL_MOUNT, "/sdcard"));
			mConstValues.insert(make_pair(TW_EXTERNAL_LABEL, "sdcard"));
			mValues.insert(make_pair(TW_ZIP_EXTERNAL_VAR, make_pair("/sdcard", 1)));
		#endif
	#endif
#endif

#ifdef TW_DEFAULT_EXTERNAL_STORAGE
	SetValue(TW_USE_EXTERNAL_STORAGE, 1);
	LOGI("Defaulting to external storage.\n");
#endif

	str = GetCurrentStoragePath();
#ifdef RECOVERY_SDCARD_ON_DATA
	#ifndef TW_EXTERNAL_STORAGE_PATH
		SetValue(TW_ZIP_LOCATION_VAR, "/sdcard", 1);
	#else
		SetValue(TW_ZIP_LOCATION_VAR, str.c_str(), 1);
	#endif
#else
	SetValue(TW_ZIP_LOCATION_VAR, str.c_str(), 1);
#endif
	str += "/TWRP/BACKUPS/";
    str += device_id;
	SetValue(TW_BACKUPS_FOLDER_VAR, str, 0);

    if (strlen(EXPAND(SP1_DISPLAY_NAME)))    mConstValues.insert(make_pair(TW_SP1_PARTITION_NAME_VAR, EXPAND(SP1_DISPLAY_NAME)));
    if (strlen(EXPAND(SP2_DISPLAY_NAME)))    mConstValues.insert(make_pair(TW_SP2_PARTITION_NAME_VAR, EXPAND(SP2_DISPLAY_NAME)));
    if (strlen(EXPAND(SP3_DISPLAY_NAME)))    mConstValues.insert(make_pair(TW_SP3_PARTITION_NAME_VAR, EXPAND(SP3_DISPLAY_NAME)));

    mConstValues.insert(make_pair(TW_REBOOT_SYSTEM, tw_isRebootCommandSupported(rb_system) ? "1" : "0"));
#ifdef TW_NO_REBOOT_RECOVERY
	mConstValues.insert(make_pair(TW_REBOOT_RECOVERY, "0"));
#else
	mConstValues.insert(make_pair(TW_REBOOT_RECOVERY, tw_isRebootCommandSupported(rb_recovery) ? "1" : "0"));
#endif
    mConstValues.insert(make_pair(TW_REBOOT_POWEROFF, tw_isRebootCommandSupported(rb_poweroff) ? "1" : "0"));
#ifdef TW_NO_REBOOT_BOOTLOADER
	mConstValues.insert(make_pair(TW_REBOOT_BOOTLOADER, "0"));
#else
	mConstValues.insert(make_pair(TW_REBOOT_BOOTLOADER, tw_isRebootCommandSupported(rb_bootloader) ? "1" : "0"));
#endif
#ifdef RECOVERY_SDCARD_ON_DATA
	mConstValues.insert(make_pair(TW_HAS_DATA_MEDIA, "1"));
#else
	mConstValues.insert(make_pair(TW_HAS_DATA_MEDIA, "0"));
#endif
#ifdef TW_NO_BATT_PERCENT
	mConstValues.insert(make_pair(TW_NO_BATTERY_PERCENT, "1"));
#else
	mConstValues.insert(make_pair(TW_NO_BATTERY_PERCENT, "0"));
#endif
#ifdef TW_CUSTOM_POWER_BUTTON
	mConstValues.insert(make_pair(TW_POWER_BUTTON, EXPAND(TW_CUSTOM_POWER_BUTTON)));
#else
	mConstValues.insert(make_pair(TW_POWER_BUTTON, "0"));
#endif
#ifdef TW_ALWAYS_RMRF
	mConstValues.insert(make_pair(TW_RM_RF_VAR, "1"));
#endif
#ifdef TW_NEVER_UNMOUNT_SYSTEM
	mConstValues.insert(make_pair(TW_DONT_UNMOUNT_SYSTEM, "1"));
#else
	mConstValues.insert(make_pair(TW_DONT_UNMOUNT_SYSTEM, "0"));
#endif
#ifdef TW_NO_USB_STORAGE
	mConstValues.insert(make_pair(TW_HAS_USB_STORAGE, "0"));
#else
	mConstValues.insert(make_pair(TW_HAS_USB_STORAGE, "1"));
#endif
#ifdef TW_INCLUDE_INJECTTWRP
	mConstValues.insert(make_pair(TW_HAS_INJECTTWRP, "1"));
	mValues.insert(make_pair(TW_INJECT_AFTER_ZIP, make_pair("1", 1)));
#else
	mConstValues.insert(make_pair(TW_HAS_INJECTTWRP, "0"));
	mValues.insert(make_pair(TW_INJECT_AFTER_ZIP, make_pair("0", 1)));
#endif
#ifdef TW_FLASH_FROM_STORAGE
	mConstValues.insert(make_pair(TW_FLASH_ZIP_IN_PLACE, "1"));
#endif
#ifdef TW_HAS_DOWNLOAD_MODE
	mConstValues.insert(make_pair(TW_DOWNLOAD_MODE, "1"));
#endif

    mConstValues.insert(make_pair(TW_MIN_SYSTEM_VAR, TW_MIN_SYSTEM_SIZE));
	mValues.insert(make_pair(TW_BACKUP_NAME, make_pair("0", 0)));
	mValues.insert(make_pair(TW_BACKUP_SYSTEM_VAR, make_pair("1", 1)));
    mValues.insert(make_pair(TW_BACKUP_DATA_VAR, make_pair("1", 1)));
    mValues.insert(make_pair(TW_BACKUP_BOOT_VAR, make_pair("1", 1)));
    mValues.insert(make_pair(TW_BACKUP_RECOVERY_VAR, make_pair("0", 1)));
    mValues.insert(make_pair(TW_BACKUP_CACHE_VAR, make_pair("0", 1)));
    mValues.insert(make_pair(TW_BACKUP_SP1_VAR, make_pair("0", 1)));
    mValues.insert(make_pair(TW_BACKUP_SP2_VAR, make_pair("0", 1)));
    mValues.insert(make_pair(TW_BACKUP_SP3_VAR, make_pair("0", 1)));
    mValues.insert(make_pair(TW_BACKUP_ANDSEC_VAR, make_pair("0", 1)));
    mValues.insert(make_pair(TW_BACKUP_SDEXT_VAR, make_pair("0", 1)));
	mValues.insert(make_pair(TW_BACKUP_SYSTEM_SIZE, make_pair("0", 0)));
	mValues.insert(make_pair(TW_BACKUP_DATA_SIZE, make_pair("0", 0)));
	mValues.insert(make_pair(TW_BACKUP_BOOT_SIZE, make_pair("0", 0)));
	mValues.insert(make_pair(TW_BACKUP_RECOVERY_SIZE, make_pair("0", 0)));
	mValues.insert(make_pair(TW_BACKUP_CACHE_SIZE, make_pair("0", 0)));
	mValues.insert(make_pair(TW_BACKUP_ANDSEC_SIZE, make_pair("0", 0)));
	mValues.insert(make_pair(TW_BACKUP_SDEXT_SIZE, make_pair("0", 0)));
	mValues.insert(make_pair(TW_BACKUP_SP1_SIZE, make_pair("0", 0)));
	mValues.insert(make_pair(TW_BACKUP_SP2_SIZE, make_pair("0", 0)));
	mValues.insert(make_pair(TW_BACKUP_SP3_SIZE, make_pair("0", 0)));
	mValues.insert(make_pair(TW_STORAGE_FREE_SIZE, make_pair("0", 0)));
	
    mValues.insert(make_pair(TW_REBOOT_AFTER_FLASH_VAR, make_pair("0", 1)));
    mValues.insert(make_pair(TW_SIGNED_ZIP_VERIFY_VAR, make_pair("0", 1)));
    mValues.insert(make_pair(TW_FORCE_MD5_CHECK_VAR, make_pair("0", 1)));
    mValues.insert(make_pair(TW_COLOR_THEME_VAR, make_pair("0", 1)));
    mValues.insert(make_pair(TW_USE_COMPRESSION_VAR, make_pair("0", 1)));
	mValues.insert(make_pair(TW_IGNORE_IMAGE_SIZE, make_pair("0", 1)));
    mValues.insert(make_pair(TW_SHOW_SPAM_VAR, make_pair("0", 1)));
    mValues.insert(make_pair(TW_TIME_ZONE_VAR, make_pair("CST6CDT", 1)));
    mValues.insert(make_pair(TW_SORT_FILES_BY_DATE_VAR, make_pair("0", 1)));
    mValues.insert(make_pair(TW_GUI_SORT_ORDER, make_pair("1", 1)));
    mValues.insert(make_pair(TW_RM_RF_VAR, make_pair("0", 1)));
    mValues.insert(make_pair(TW_SKIP_MD5_CHECK_VAR, make_pair("0", 1)));
    mValues.insert(make_pair(TW_SKIP_MD5_GENERATE_VAR, make_pair("0", 1)));
    mValues.insert(make_pair(TW_SDEXT_SIZE, make_pair("512", 1)));
    mValues.insert(make_pair(TW_SWAP_SIZE, make_pair("32", 1)));
    mValues.insert(make_pair(TW_SDPART_FILE_SYSTEM, make_pair("ext3", 1)));
    mValues.insert(make_pair(TW_TIME_ZONE_GUISEL, make_pair("CST6;CDT", 1)));
    mValues.insert(make_pair(TW_TIME_ZONE_GUIOFFSET, make_pair("0", 1)));
    mValues.insert(make_pair(TW_TIME_ZONE_GUIDST, make_pair("1", 1)));
    mValues.insert(make_pair(TW_ACTION_BUSY, make_pair("0", 0)));
    mValues.insert(make_pair(TW_BACKUP_AVG_IMG_RATE, make_pair("15000000", 1)));
    mValues.insert(make_pair(TW_BACKUP_AVG_FILE_RATE, make_pair("3000000", 1)));
    mValues.insert(make_pair(TW_BACKUP_AVG_FILE_COMP_RATE, make_pair("2000000", 1)));
    mValues.insert(make_pair(TW_RESTORE_AVG_IMG_RATE, make_pair("15000000", 1)));
    mValues.insert(make_pair(TW_RESTORE_AVG_FILE_RATE, make_pair("3000000", 1)));
    mValues.insert(make_pair(TW_RESTORE_AVG_FILE_COMP_RATE, make_pair("2000000", 1)));
	if (GetIntValue(TW_HAS_INTERNAL) == 1 && GetIntValue(TW_HAS_DATA_MEDIA) == 1 && GetIntValue(TW_HAS_EXTERNAL) == 0)
		SetValue(TW_HAS_USB_STORAGE, 0, 0);
	else
		SetValue(TW_HAS_USB_STORAGE, 1, 0);
	mValues.insert(make_pair(TW_ZIP_INDEX, make_pair("0", 0)));
	mValues.insert(make_pair(TW_ZIP_QUEUE_COUNT, make_pair("0", 0)));
	mValues.insert(make_pair(TW_FILENAME, make_pair("/sdcard", 0)));
	mValues.insert(make_pair(TW_SIMULATE_ACTIONS, make_pair("0", 1)));
	mValues.insert(make_pair(TW_SIMULATE_FAIL, make_pair("0", 1)));
}

// Magic Values
int DataManager::GetMagicValue(const string varName, string& value)
{
    // Handle special dynamic cases
    if (varName == "tw_time")
    {
        char tmp[32];

        struct tm *current;
        time_t now;
        now = time(0);
        current = localtime(&now);

        if (current->tm_hour >= 12)
            sprintf(tmp, "%d:%02d PM", current->tm_hour == 12 ? 12 : current->tm_hour - 12, current->tm_min);
        else
            sprintf(tmp, "%d:%02d AM", current->tm_hour == 0 ? 12 : current->tm_hour, current->tm_min);

        value = tmp;
        return 0;
    }
    if (varName == "tw_battery")
    {
        char tmp[16];

        sprintf(tmp, "%i%%", get_battery_level());
        value = tmp;
        return 0;
    }
    return -1;
}

string DataManager::GetCurrentStoragePath(void)
{
	if (GetIntValue(TW_HAS_DUAL_STORAGE) == 1) {
		if (GetIntValue(TW_USE_EXTERNAL_STORAGE) == 0)
			return GetStrValue(TW_INTERNAL_PATH);
		else
			return GetStrValue(TW_EXTERNAL_PATH);
	} else if (GetIntValue(TW_HAS_INTERNAL) == 1)
		return GetStrValue(TW_INTERNAL_PATH);
	else
		return GetStrValue(TW_EXTERNAL_PATH);
}

string& DataManager::CGetCurrentStoragePath()
{
	if (GetIntValue(TW_HAS_DUAL_STORAGE) == 1) {
		if (GetIntValue(TW_USE_EXTERNAL_STORAGE) == 0)
			return GetValueRef(TW_INTERNAL_PATH);
		else
			return GetValueRef(TW_EXTERNAL_PATH);
	} else if (GetIntValue(TW_HAS_INTERNAL) == 1)
		return GetValueRef(TW_INTERNAL_PATH);
	else
		return GetValueRef(TW_EXTERNAL_PATH);
}

string DataManager::GetCurrentStorageMount(void)
{
	if (GetIntValue(TW_HAS_DUAL_STORAGE) == 1) {
		if (GetIntValue(TW_USE_EXTERNAL_STORAGE) == 0)
			return GetStrValue(TW_INTERNAL_MOUNT);
		else
			return GetStrValue(TW_EXTERNAL_MOUNT);
	} else if (GetIntValue(TW_HAS_INTERNAL) == 1)
		return GetStrValue(TW_INTERNAL_MOUNT);
	else
		return GetStrValue(TW_EXTERNAL_MOUNT);
}

string& DataManager::CGetCurrentStorageMount()
{
	if (GetIntValue(TW_HAS_DUAL_STORAGE) == 1) {
		if (GetIntValue(TW_USE_EXTERNAL_STORAGE) == 0)
			return GetValueRef(TW_INTERNAL_MOUNT);
		else
			return GetValueRef(TW_EXTERNAL_MOUNT);
	} else if (GetIntValue(TW_HAS_INTERNAL) == 1)
		return GetValueRef(TW_INTERNAL_MOUNT);
	else
		return GetValueRef(TW_EXTERNAL_MOUNT);
}

string DataManager::GetSettingsStoragePath(void)
{
	if (GetIntValue(TW_HAS_INTERNAL) == 1)
		return GetStrValue(TW_INTERNAL_PATH);
	else
		return GetStrValue(TW_EXTERNAL_PATH);
}

string& DataManager::CGetSettingsStoragePath()
{
	if (GetIntValue(TW_HAS_INTERNAL) == 1)
		return GetValueRef(TW_INTERNAL_PATH);
	else
		return GetValueRef(TW_EXTERNAL_PATH);
}

string DataManager::GetSettingsStorageMount(void)
{
	if (GetIntValue(TW_HAS_INTERNAL) == 1)
		return GetStrValue(TW_INTERNAL_MOUNT);
	else
		return GetStrValue(TW_EXTERNAL_MOUNT);
}

string& DataManager::CGetSettingsStorageMount()
{
	if (GetIntValue(TW_HAS_INTERNAL) == 1)
		return GetValueRef(TW_INTERNAL_MOUNT);
	else
		return GetValueRef(TW_EXTERNAL_MOUNT);
}

extern "C" int DataManager_ResetDefaults()
{
    return DataManager::ResetDefaults();
}

extern "C" void DataManager_LoadDefaults()
{
    return DataManager::SetDefaultValues();
}

extern "C" int DataManager_LoadValues(const char* filename)
{
    return DataManager::LoadValues(filename);
}

extern "C" int DataManager_Flush()
{
    return DataManager::Flush();
}

extern "C" int DataManager_GetValue(const char* varName, char* value)
{
    int ret;
    string str;

    ret = DataManager::GetValue(varName, str);
    if (ret == 0)
        strcpy(value, str.c_str());
    return ret;
}

extern "C" const char* DataManager_GetStrValue(const char* varName)
{
    string& str = DataManager::GetValueRef(varName);
    return str.c_str();
}

extern "C" const char* DataManager_GetCurrentStoragePath(void)
{
    string& str = DataManager::CGetCurrentStoragePath();
    return str.c_str();
}

extern "C" const char* DataManager_GetSettingsStoragePath(void)
{
    string& str = DataManager::CGetSettingsStoragePath();
    return str.c_str();
}

extern "C" const char* DataManager_GetCurrentStorageMount(void)
{
    string& str = DataManager::CGetCurrentStorageMount();
    return str.c_str();
}

extern "C" const char* DataManager_GetSettingsStorageMount(void)
{
    string& str = DataManager::CGetSettingsStorageMount();
    return str.c_str();
}

extern "C" int DataManager_GetIntValue(const char* varName)
{
    return DataManager::GetIntValue(varName);
}

extern "C" int DataManager_SetStrValue(const char* varName, char* value)
{
    return DataManager::SetValue(varName, value, 0);
}

extern "C" int DataManager_SetIntValue(const char* varName, int value)
{
    return DataManager::SetValue(varName, value, 0);
}

extern "C" int DataManager_SetFloatValue(const char* varName, float value)
{
    return DataManager::SetValue(varName, value, 0);
}

extern "C" int DataManager_ToggleIntValue(const char* varName)
{
    if (DataManager::GetIntValue(varName))
        return DataManager::SetValue(varName, 0);
    else
        return DataManager::SetValue(varName, 1);
}

extern "C" void DataManager_DumpValues()
{
    return DataManager::DumpValues();
}

