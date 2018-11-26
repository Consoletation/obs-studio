#include <obs-module.h>
#include <stdio.h>

#include <media-io/audio-math.h>
#include <math.h>

#include <windows.h>
#include "circle-buffer.h"
#include "VoicemeeterRemote.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("rematrix-filter", "en-US")

#define blog(level, msg, ...) blog(level, "obs-voicemeeter: " msg, ##__VA_ARGS__)

#define MT_ obs_module_text

#ifndef MAX_AUDIO_SIZE
#ifndef AUDIO_OUTPUT_FRAMES
#define	AUDIO_OUTPUT_FRAMES 1024
#endif
#define	MAX_AUDIO_SIZE (AUDIO_OUTPUT_FRAMES * sizeof(float))
#endif // !MAX_AUDIO_SIZE

enum audio_format get_planar_format(audio_format format)
{
	if (is_audio_planar(format))
		return format;

	switch (format) {
	case AUDIO_FORMAT_U8BIT:
		return AUDIO_FORMAT_U8BIT_PLANAR;
	case AUDIO_FORMAT_16BIT:
		return AUDIO_FORMAT_16BIT_PLANAR;
	case AUDIO_FORMAT_32BIT:
		return AUDIO_FORMAT_32BIT_PLANAR;
	case AUDIO_FORMAT_FLOAT:
		return AUDIO_FORMAT_FLOAT_PLANAR;
	default:
		return AUDIO_FORMAT_UNKNOWN;
	}
}

static enum {
	voicemeeter_normal = 1,
	voicemeeter_banana = 2,
	voicemeeter_potato = 3,
};

static enum {
	voicemeeter_insert_in = 0,
	voicemeeter_insert_out,
	voicemeeter_main
};

static char uninstDirKey[] = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

#define INSTALLER_UNINST_KEY   "VB:Voicemeeter {17359A74-1236-5467}"

struct VBVMR_T_AUDIOBUFFER_TS {
	VBVMR_T_AUDIOBUFFER data;
	uint64_t ts;
};

static StreamableBuffer<VBVMR_T_AUDIOBUFFER_TS> OBSBufferInsertIn;
static StreamableBuffer<VBVMR_T_AUDIOBUFFER_TS> OBSBufferInsertOut;
static StreamableBuffer<VBVMR_T_AUDIOBUFFER_TS> OBSBufferMain;

void RemoveNameInPath(char * szPath)
{
	long ll;
	ll = (long)strlen(szPath);
	while ((ll > 0) && (szPath[ll] != '\\')) ll--;
	if (szPath[ll] == '\\') szPath[ll] = 0;
}

#ifndef KEY_WOW64_32KEY
#define KEY_WOW64_32KEY 0x0200
#endif

BOOL __cdecl RegistryGetVoicemeeterFolderA(char * szDir)
{
	char szKey[256];
	char sss[1024];
	DWORD nnsize = 1024;
	HKEY hkResult;
	LONG rep;
	DWORD pptype = REG_SZ;
	sss[0] = 0;

	// build Voicemeeter uninstallation key
	strcpy(szKey, uninstDirKey);
	strcat(szKey, "\\");
	strcat(szKey, INSTALLER_UNINST_KEY);

	// open key
	rep = RegOpenKeyExA(HKEY_LOCAL_MACHINE, szKey, 0, KEY_READ, &hkResult);
	if (rep != ERROR_SUCCESS) {
		// if not present we consider running in 64bit mode and force to read 32bit registry
		rep = RegOpenKeyExA(HKEY_LOCAL_MACHINE, szKey, 0, KEY_READ | KEY_WOW64_32KEY, &hkResult);
	}

	if (rep != ERROR_SUCCESS) {
		blog(LOG_ERROR, "error %i reading registry", rep);
		return FALSE;
	}
	// read uninstall from program path
	rep = RegQueryValueExA(hkResult, "UninstallString", 0, &pptype, (unsigned char *)sss, &nnsize);
	RegCloseKey(hkResult);

	if (pptype != REG_SZ) {
		blog(LOG_INFO, "pptype %u, %u", pptype, REG_SZ);
		return FALSE;
	}
	if (rep != ERROR_SUCCESS) {
		blog(LOG_ERROR, "error %i getting value", rep);
		return FALSE;
	}
	// remove name to get the path only
	RemoveNameInPath(sss);
	if (nnsize > 512) nnsize = 512;
	strncpy(szDir, sss, nnsize);

	return TRUE;
}

static T_VBVMR_INTERFACE iVMR;
static HMODULE G_H_Module = NULL;
static long application = 0;
struct version32_t {
	union {
		struct {
			uint8_t v1, v2, v3, v4;
		};
		long v;
	};
};
static version32_t version = { 0 };
static long vb_type;
static std::vector<int32_t> validInputs = {0, 12, 22, 34 };
static std::vector<int32_t> validOutputs = {0, 16, 40, 64 };
static std::vector<int32_t> validMains = {0, 28, 62, 98 };

static void copyToBuffer(VBVMR_T_AUDIOBUFFER_TS &buf, VBVMR_T_AUDIOBUFFER_TS &out, bool used)
{
	size_t bufSize = buf.data.audiobuffer_nbs * sizeof(float);
	if (used) {
		size_t oldBufSize = out.data.audiobuffer_nbs * sizeof(float);
		if (bufSize > oldBufSize) {
			for (int i = 0; i < buf.data.audiobuffer_nbi; i++) {
				bfree(out.data.audiobuffer_r[i]);
				out.data.audiobuffer_r[i] = (float*)bmemdup(buf.data.audiobuffer_r[i], bufSize);
			}
			for (int i = 0; i < buf.data.audiobuffer_nbo; i++) {
				bfree(out.data.audiobuffer_w[i]);
				out.data.audiobuffer_w[i] = (float*)bmemdup(buf.data.audiobuffer_w[i], bufSize);
			}
		} else {
			for (int i = 0; i < buf.data.audiobuffer_nbi; i++)
				memcpy(out.data.audiobuffer_r[i], buf.data.audiobuffer_r[i], bufSize);
			for (int i = 0; i < buf.data.audiobuffer_nbo; i++)
				memcpy(out.data.audiobuffer_w[i], buf.data.audiobuffer_w[i], bufSize);

		}
	} else {
		for (int i = 0; i < buf.data.audiobuffer_nbi; i++)
			out.data.audiobuffer_r[i] = (float*)bmemdup(buf.data.audiobuffer_r[i], bufSize);
		for (int i = 0; i < buf.data.audiobuffer_nbo; i++)
			out.data.audiobuffer_w[i] = (float*)bmemdup(buf.data.audiobuffer_w[i], bufSize);
	}
	out.data.audiobuffer_nbi = buf.data.audiobuffer_nbi;
	out.data.audiobuffer_nbo = buf.data.audiobuffer_nbo;
	out.data.audiobuffer_nbs = buf.data.audiobuffer_nbs;
	out.data.audiobuffer_sr = buf.data.audiobuffer_sr;
	out.ts = buf.ts;
}

static void writeInsertAudio(VBVMR_T_AUDIOBUFFER_TS &buf, VBVMR_T_AUDIOBUFFER_TS &out, bool used)
{
	copyToBuffer(buf, out, used);
	size_t bufSize = buf.data.audiobuffer_nbs * sizeof(float);
	/*pass-through*/
	for (int i = 0; i < validInputs[vb_type]; i++)
		memcpy(buf.data.audiobuffer_w[i], buf.data.audiobuffer_r[i], bufSize);
}

static void writeInsertOutAudio(VBVMR_T_AUDIOBUFFER_TS &buf, VBVMR_T_AUDIOBUFFER_TS &out, bool used)
{
	copyToBuffer(buf, out, used);
	size_t bufSize = buf.data.audiobuffer_nbs * sizeof(float);
	/*pass-through*/
	for (int i = 0; i < validOutputs[vb_type]; i++)
		memcpy(buf.data.audiobuffer_w[i], buf.data.audiobuffer_r[i], bufSize);
}

static void writeMainAudio(VBVMR_T_AUDIOBUFFER_TS &buf, VBVMR_T_AUDIOBUFFER_TS &out, bool used)
{
	copyToBuffer(buf, out, used);
	size_t bufSize = buf.data.audiobuffer_nbs * sizeof(float);
	/*pass-through*/
	for (int i = 0; i < validOutputs[vb_type]; i++)
		memcpy(buf.data.audiobuffer_w[i], buf.data.audiobuffer_r[i+validInputs[vb_type]], bufSize);
}

static long audioCallback(void *lpUser, long nCommand, void *lpData, long nnn)
{
	UNUSED_PARAMETER(lpUser);
	uint64_t tStamp = os_gettime_ns();
	float *bufferIn;
	float *bufferOut;
	VBVMR_LPT_AUDIOINFO audioInfo;
	VBVMR_T_AUDIOBUFFER_TS audioBuf;
	//VBVMR_LPT_AUDIOBUFFER audioBuf;

	switch (nCommand) {
	case VBVMR_CBCOMMAND_STARTING:
		audioInfo = (VBVMR_LPT_AUDIOINFO)lpData;
		break;
	case VBVMR_CBCOMMAND_CHANGE:
		audioInfo = (VBVMR_LPT_AUDIOINFO)lpData;
		break;
	case VBVMR_CBCOMMAND_ENDING:
		break;
	case VBVMR_CBCOMMAND_BUFFER_IN:
		audioBuf.data = *((VBVMR_LPT_AUDIOBUFFER)lpData);
		audioBuf.ts = tStamp;
		OBSBufferInsertIn.Write(audioBuf, writeInsertAudio);
		break;
	case VBVMR_CBCOMMAND_BUFFER_OUT:
		audioBuf.data = *((VBVMR_LPT_AUDIOBUFFER)lpData);
		audioBuf.ts = tStamp;
		OBSBufferInsertOut.Write(audioBuf, writeInsertOutAudio);
		break;
	case VBVMR_CBCOMMAND_BUFFER_MAIN:
		audioBuf.data = *((VBVMR_LPT_AUDIOBUFFER)lpData);
		audioBuf.ts = tStamp;
		OBSBufferMain.Write(audioBuf, writeMainAudio);
		break;
	}
	return 0;
}

long InitializeDLLInterfaces(void)
{
	char szDllName[1024] = {0};
	memset(&iVMR, 0, sizeof(T_VBVMR_INTERFACE));

	if (RegistryGetVoicemeeterFolderA(szDllName) == FALSE) {
		// voicemeeter not installed?
		blog(LOG_INFO, "voicemeeter does not appear to be installed");
		return -100;
	}

	//use right dll w/ bitness
	if (sizeof(void*) == 8) strcat(szDllName, "\\VoicemeeterRemote64.dll");
	else strcat(szDllName, "\\VoicemeeterRemote.dll");

	// Load Dll
	G_H_Module = LoadLibraryA(szDllName);
	if (G_H_Module == NULL) {
		blog(LOG_INFO, ".dll failed to load");
		return -101;
	}

	// Get function pointers
	iVMR.VBVMR_Login = (T_VBVMR_Login)GetProcAddress(G_H_Module, "VBVMR_Login");
	iVMR.VBVMR_Logout = (T_VBVMR_Logout)GetProcAddress(G_H_Module, "VBVMR_Logout");
	iVMR.VBVMR_RunVoicemeeter = (T_VBVMR_RunVoicemeeter)GetProcAddress(G_H_Module, "VBVMR_RunVoicemeeter");
	iVMR.VBVMR_GetVoicemeeterType = (T_VBVMR_GetVoicemeeterType)GetProcAddress(G_H_Module, "VBVMR_GetVoicemeeterType");
	iVMR.VBVMR_GetVoicemeeterVersion = (T_VBVMR_GetVoicemeeterVersion)GetProcAddress(G_H_Module, "VBVMR_GetVoicemeeterVersion");

	iVMR.VBVMR_IsParametersDirty = (T_VBVMR_IsParametersDirty)GetProcAddress(G_H_Module, "VBVMR_IsParametersDirty");
	iVMR.VBVMR_GetParameterFloat = (T_VBVMR_GetParameterFloat)GetProcAddress(G_H_Module, "VBVMR_GetParameterFloat");
	iVMR.VBVMR_GetParameterStringA = (T_VBVMR_GetParameterStringA)GetProcAddress(G_H_Module, "VBVMR_GetParameterStringA");
	iVMR.VBVMR_GetParameterStringW = (T_VBVMR_GetParameterStringW)GetProcAddress(G_H_Module, "VBVMR_GetParameterStringW");
	iVMR.VBVMR_GetLevel = (T_VBVMR_GetLevel)GetProcAddress(G_H_Module, "VBVMR_GetLevel");
	iVMR.VBVMR_GetMidiMessage = (T_VBVMR_GetMidiMessage)GetProcAddress(G_H_Module, "VBVMR_GetMidiMessage");

	iVMR.VBVMR_SetParameterFloat = (T_VBVMR_SetParameterFloat)GetProcAddress(G_H_Module, "VBVMR_SetParameterFloat");
	iVMR.VBVMR_SetParameters = (T_VBVMR_SetParameters)GetProcAddress(G_H_Module, "VBVMR_SetParameters");
	iVMR.VBVMR_SetParametersW = (T_VBVMR_SetParametersW)GetProcAddress(G_H_Module, "VBVMR_SetParametersW");
	iVMR.VBVMR_SetParameterStringA = (T_VBVMR_SetParameterStringA)GetProcAddress(G_H_Module, "VBVMR_SetParameterStringA");
	iVMR.VBVMR_SetParameterStringW = (T_VBVMR_SetParameterStringW)GetProcAddress(G_H_Module, "VBVMR_SetParameterStringW");

	iVMR.VBVMR_Output_GetDeviceNumber = (T_VBVMR_Output_GetDeviceNumber)GetProcAddress(G_H_Module, "VBVMR_Output_GetDeviceNumber");
	iVMR.VBVMR_Output_GetDeviceDescA = (T_VBVMR_Output_GetDeviceDescA)GetProcAddress(G_H_Module, "VBVMR_Output_GetDeviceDescA");
	iVMR.VBVMR_Output_GetDeviceDescW = (T_VBVMR_Output_GetDeviceDescW)GetProcAddress(G_H_Module, "VBVMR_Output_GetDeviceDescW");
	iVMR.VBVMR_Input_GetDeviceNumber = (T_VBVMR_Input_GetDeviceNumber)GetProcAddress(G_H_Module, "VBVMR_Input_GetDeviceNumber");
	iVMR.VBVMR_Input_GetDeviceDescA = (T_VBVMR_Input_GetDeviceDescA)GetProcAddress(G_H_Module, "VBVMR_Input_GetDeviceDescA");
	iVMR.VBVMR_Input_GetDeviceDescW = (T_VBVMR_Input_GetDeviceDescW)GetProcAddress(G_H_Module, "VBVMR_Input_GetDeviceDescW");

	iVMR.VBVMR_AudioCallbackRegister = (T_VBVMR_AudioCallbackRegister)GetProcAddress(G_H_Module, "VBVMR_AudioCallbackRegister");
	iVMR.VBVMR_AudioCallbackStart = (T_VBVMR_AudioCallbackStart)GetProcAddress(G_H_Module, "VBVMR_AudioCallbackStart");
	iVMR.VBVMR_AudioCallbackStop = (T_VBVMR_AudioCallbackStop)GetProcAddress(G_H_Module, "VBVMR_AudioCallbackStop");
	iVMR.VBVMR_AudioCallbackUnregister = (T_VBVMR_AudioCallbackUnregister)GetProcAddress(G_H_Module, "VBVMR_AudioCallbackUnregister");

	// Check pointers are valid
	if (iVMR.VBVMR_Login == NULL) return -1;
	if (iVMR.VBVMR_Logout == NULL) return -2;
	if (iVMR.VBVMR_RunVoicemeeter == NULL) return -2;
	if (iVMR.VBVMR_GetVoicemeeterType == NULL) return -3;
	if (iVMR.VBVMR_GetVoicemeeterVersion == NULL) return -4;
	if (iVMR.VBVMR_IsParametersDirty == NULL) return -5;
	if (iVMR.VBVMR_GetParameterFloat == NULL) return -6;
	if (iVMR.VBVMR_GetParameterStringA == NULL) return -7;
	if (iVMR.VBVMR_GetParameterStringW == NULL) return -8;
	if (iVMR.VBVMR_GetLevel == NULL) return -9;
	if (iVMR.VBVMR_SetParameterFloat == NULL) return -10;
	if (iVMR.VBVMR_SetParameters == NULL) return -11;
	if (iVMR.VBVMR_SetParametersW == NULL) return -12;
	if (iVMR.VBVMR_SetParameterStringA == NULL) return -13;
	if (iVMR.VBVMR_SetParameterStringW == NULL) return -14;
	if (iVMR.VBVMR_GetMidiMessage == NULL) return -15;

	if (iVMR.VBVMR_Output_GetDeviceNumber == NULL) return -30;
	if (iVMR.VBVMR_Output_GetDeviceDescA == NULL) return -31;
	if (iVMR.VBVMR_Output_GetDeviceDescW == NULL) return -32;
	if (iVMR.VBVMR_Input_GetDeviceNumber == NULL) return -33;
	if (iVMR.VBVMR_Input_GetDeviceDescA == NULL) return -34;
	if (iVMR.VBVMR_Input_GetDeviceDescW == NULL) return -35;

	if (iVMR.VBVMR_AudioCallbackRegister == NULL) return -40;
	if (iVMR.VBVMR_AudioCallbackStart == NULL) return -41;
	if (iVMR.VBVMR_AudioCallbackStop == NULL) return -42;
	if (iVMR.VBVMR_AudioCallbackUnregister == NULL) return -43;

	return 0;
}

static int voicemeeter_channel_count;

class vi_data : public StreamableReader<VBVMR_T_AUDIOBUFFER_TS> {
	std::string _name;
	obs_data_t *_settings;
	obs_source_t *_source;
	std::vector<int16_t> _route;
	std::vector<uint8_t> _silentBuffer;
	int _maxChannels;
	enum speaker_layout _layout;
	int _stage;
	
//	enum speaker_layout {
//		SPEAKERS_UNKNOWN,   /**< Unknown setting, fallback is stereo. */
//		SPEAKERS_MONO,      /**< Channels: MONO */
//		SPEAKERS_STEREO,    /**< Channels: FL, FR */
//		SPEAKERS_2POINT1,   /**< Channels: FL, FR, LFE */
//		SPEAKERS_4POINT0,   /**< Channels: FL, FR, FC, RC */
//		SPEAKERS_4POINT1,   /**< Channels: FL, FR, FC, LFE, RC */
//		SPEAKERS_5POINT1,   /**< Channels: FL, FR, FC, LFE, RL, RR */
//		SPEAKERS_7POINT1 = 8, /**< Channels: FL, FR, FC, LFE, RL, RR, SL, SR */
//	};
public:
	vi_data(obs_data_t *settings = nullptr, obs_source_t *source = nullptr) :
		_settings(settings), _source(source)
	{
		_route.reserve(MAX_AV_PLANES);
		_silentBuffer.reserve(MAX_AUDIO_SIZE);
		for (int i = 0; i < MAX_AV_PLANES; i++) {
			_route.push_back(-1);
		}
		for (int i = 0; i < MAX_AUDIO_SIZE; i++) {
			_silentBuffer.push_back(0);
		}
		update(settings);
	}

	~vi_data()
	{

	}

	std::string Name()
	{
		return _name;
	}

	std::string Name(std::string name)
	{
		return (_name = name);
	}

	void update(obs_data_t *settings)
	{
		if (!settings)
			return;
		_layout = (enum speaker_layout)obs_data_get_int(settings, "layout");
		for (int i = 0; i < MAX_AV_PLANES; i++) {
			std::string name = "route " + std::to_string(i);
			_route[i] = (int16_t)obs_data_get_int(settings, name.c_str());
		}
		int nStage = obs_data_get_int(settings, "stage");
		if (_stage != nStage) {
			Disconnect();
			_stage = nStage;
			switch (_stage) {
			case voicemeeter_insert_in:
				OBSBufferInsertIn.AddListener(this);
				break;
			case voicemeeter_insert_out:
				OBSBufferInsertOut.AddListener(this);
				break;
			case voicemeeter_main:
				OBSBufferMain.AddListener(this);
				break;
			default:
				Disconnect();
			}
		}

		return;
	}

	static void fillLayouts(obs_property_t *list)
	{
		std::string name = "";
		obs_property_list_clear(list);
		for (int i = 0; i < 9; i++) {
			switch (i) {
			//case SPEAKERS_UNKNOWN:   /**< Unknown setting, fallback is stereo. */
			//	name = "None"
			case SPEAKERS_MONO:      /**< Channels: MONO */
				name = obs_module_text("Mono");
				break;
			case SPEAKERS_STEREO:    /**< Channels: FL, FR */
				name = obs_module_text("Stereo");
				break;
			case SPEAKERS_2POINT1:   /**< Channels: FL, FR, LFE */
				name = "2.1";
				break;
			case SPEAKERS_4POINT0:   /**< Channels: FL, FR, FC, RC */
				name = "4.0";
				break;
			case SPEAKERS_4POINT1:   /**< Channels: FL, FR, FC, LFE, RC */
				name = "4.1";
				break;
			case SPEAKERS_5POINT1:   /**< Channels: FL, FR, FC, LFE, RL, RR */
				name = "5.1";
				break;
			case SPEAKERS_7POINT1:   /**< Channels: FL, FR, FC, LFE, RL, RR, SL, SR */
				name = "7.1";
				break;
			default:
				continue;
			}
			obs_property_list_add_int(list, name.c_str(), i);
		}
	}

	static bool channelsModified(obs_properties_t *props, obs_property_t *list,
		obs_data_t *settings)
	{
		obs_property_list_clear(list);
		obs_property_list_add_int(list, obs_module_text("Mute"), -1);
		int stage = obs_data_get_int(settings, "stage");

		long type;
		int ret = iVMR.VBVMR_GetVoicemeeterType(&type);
		if (ret != 0) {
			vb_type = 0;
			return true;
		}
		int inputs = 0;
		int outputs = 0;
		int total = 0;
		vb_type = type;

		inputs = validInputs[type];
		outputs = validOutputs[type];

		std::string name;
		switch (stage) {
		case voicemeeter_insert_in:
			name = "Insert (input) ";
			total = inputs;
			break;
		case voicemeeter_insert_out:
			name = "Insert (output) ";
			total = outputs;
			break;
		case voicemeeter_main:
			name = "Main ";
			total = outputs;
			break;
		default:
			return true;
		};

		int i;
		
		for (i = 0; i < total; i++) {
			obs_property_list_add_int(list, (name + std::to_string(i)).c_str(), i);
		}

		return true;
	}

	static bool stageChanged(obs_properties_t *props, obs_property_t *list,
		obs_data_t *settings)
	{
		obs_property_t* pn = obs_properties_first(props);
		/* single pass over properties */
		do {
			const char* name = obs_property_name(pn);
			if (strncmp("route ", name, 6) == 0) {
				//obs_property_list_clear(pn);
				channelsModified(props, pn, settings);
			}
		} while (obs_property_next(&pn));

		return true;
	}

	static bool layoutChanged(obs_properties_t *props, obs_property_t *list,
			obs_data_t *settings)
	{
		obs_property_t *route[MAX_AUDIO_CHANNELS];
		enum speaker_layout layout = (enum speaker_layout)obs_data_get_int(settings, obs_property_name(list));
		int channels = get_audio_channels(layout);

		obs_property_t* pn = obs_properties_first(props);
		/* single pass over properties */
		int i = 0;
		do {
			const char* name = obs_property_name(pn);
			if (strncmp("route ", name, 6) == 0) {
				std::string in = (name + 6);
				i = std::stoi(in);
				obs_property_set_visible(pn, i < channels);
			}
		} while (obs_property_next(&pn));

		return true;
	}

	obs_properties_t * get_properties()
	{
		obs_properties_t *props = obs_properties_create();
		obs_property_t *prop = nullptr;
		obs_property_t *stageProperty = obs_properties_add_list(props, "stage", obs_module_text("Stage"), OBS_COMBO_TYPE_LIST,
			OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(stageProperty, obs_module_text("Voicemeeter Insert (input)"), 0);
		obs_property_list_add_int(stageProperty, obs_module_text("Voicemeeter Insert (output)"), 1);
		obs_property_list_add_int(stageProperty, obs_module_text("Voicemeeter Main"), 2);
		obs_property_set_modified_callback(stageProperty, stageChanged);

		obs_property_t *layoutProperty = obs_properties_add_list(props, "layout", obs_module_text("Speaker Layout"),
				OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_set_modified_callback(layoutProperty, layoutChanged);
		fillLayouts(layoutProperty);
		for (int i = 0; i < MAX_AV_PLANES; i++) {
			prop = obs_properties_add_list(props, ("route " + std::to_string(i)).c_str(),
				obs_module_text(("Route." + std::to_string(i)).c_str()),
				OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
			obs_property_set_visible(prop, i < get_audio_channels(_layout));
			//obs_property_set_modified_callback(prop, channelsModified);
		}
		return props;
	}

	void Read(const VBVMR_T_AUDIOBUFFER_TS *buf)
	{
		struct obs_source_audio out;
		out.timestamp = buf->ts;
		int i;

		int limit;
		switch (vb_type) {
		case voicemeeter_potato:
		case voicemeeter_banana:
		case voicemeeter_normal:
			break;
		default:
			return;
		}

		switch (_stage) {
		case voicemeeter_insert_in:
			limit = validInputs[vb_type];
			break;
		case voicemeeter_insert_out:
			limit = validOutputs[vb_type];
			break;
		case voicemeeter_main:
			limit = validOutputs[vb_type];
			break;
		default:
			return;
		}

		_maxChannels = min((MAX_AV_PLANES), (get_audio_channels(_layout)));
		for (i = 0; i < _maxChannels; i++) {
			if (_route[i] >= 0 && _route[i] < limit) {
				out.data[i] = (const uint8_t*)buf->data.audiobuffer_r[_route[i]];
			} else {
				if (buf->data.audiobuffer_nbs * 4 > _silentBuffer.size()) {
					_silentBuffer.reserve(buf->data.audiobuffer_nbs * 4);
					do {
						_silentBuffer.push_back(0);
					} while (buf->data.audiobuffer_nbs * 4 > _silentBuffer.size());
				}
				out.data[i] = (const uint8_t*)_silentBuffer.data();
			}
		}
		float check = out.data[0][0];

		out.samples_per_sec = buf->data.audiobuffer_sr;
		out.frames = buf->data.audiobuffer_nbs;
		out.format = AUDIO_FORMAT_FLOAT_PLANAR;
		out.speakers = _layout;

		obs_source_output_audio(_source, &out);
	}
};

static void *vi_create(obs_data_t *settings, obs_source_t *source)
{
	vi_data *data = new vi_data(settings, source);
	return data;
}

static void vi_destroy(void *vptr)
{
	vi_data *data = static_cast<vi_data*>(vptr);
	data->Disconnect();
	delete data;
}

static void vi_update(void *vptr, obs_data_t *settings)
{
	vi_data *data = static_cast<vi_data*>(vptr);
	data->update(settings);
}

static const char *vi_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Voicemeeter");
}

static obs_properties_t *vi_get_properties(void *vptr)
{
	vi_data *data = static_cast<vi_data*>(vptr);
	return data->get_properties();
}

static void vi_get_defaults(obs_data_t *settings)
{
	/*Mute by default*/
	for (int i = 0; i < MAX_AV_PLANES; i++) {
		std::string name = "route " + std::to_string(i);
		obs_data_set_default_int(settings, name.c_str(), -1);
	}
}

bool obs_module_load(void)
{
	int ret = InitializeDLLInterfaces();
	if (ret != 0) {
		blog(LOG_INFO, ".dll failed to be initalized");
		return false;
	}

	ret = iVMR.VBVMR_Login();
	switch (ret) {
	case 0:
		blog(LOG_INFO, "client logged in");
		break;
	case 1:
		blog(LOG_INFO, "attempting to open voicemeeter");
		ret = iVMR.VBVMR_RunVoicemeeter(voicemeeter_potato);
		if (ret == 0) {
			blog(LOG_INFO, "successfully opened potato");
			break;
		}

		ret = iVMR.VBVMR_RunVoicemeeter(voicemeeter_banana);
		if (ret == 0) {
			blog(LOG_INFO, "successfully opened banana");
			break;
		}
		
		ret = iVMR.VBVMR_RunVoicemeeter(voicemeeter_normal);
		if (ret == 0) {
			blog(LOG_INFO, "successfully opened basic");
			break;
		}
		
		blog(LOG_INFO, "failed to open voicemeeter");
		return false;
	case -1:
		blog(LOG_ERROR, "cannot get client");
		return false;
	case -2:
		blog(LOG_ERROR, "unexpected login");
		return false;
	}

	ret = iVMR.VBVMR_GetVoicemeeterType((long *)&application);
	if (ret != 0) {
		blog(LOG_ERROR, "could not get voicmeeter type");
		return false;
	}
	ret = iVMR.VBVMR_GetVoicemeeterVersion((long *)&version.v);
	if (ret != 0) {
		blog(LOG_ERROR, "could not get voicmeeter version");
		return false;
	}

	switch (application) {
	case voicemeeter_potato:
		blog(LOG_INFO, "running voicemeeter potato %u.%u.%u.%u",
			version.v4, version.v3, version.v2, version.v1);
		break;
	case voicemeeter_banana:
		blog(LOG_INFO, "running voicemeeter banana %u.%u.%u.%u",
				version.v4, version.v3, version.v2, version.v1);
		break;
	case voicemeeter_normal:
		blog(LOG_INFO, "running voicemeeter %u.%u.%u.%u",
				version.v4, version.v3, version.v2, version.v1);
		break;
	default:
		blog(LOG_ERROR, "unknown voicemeeter version");
		return false;
	}
	vb_type = application;

	long deviceType;
	char deviceName[1024] = { 0 };
	char deviceId[1024] = { 0 };

	int num = iVMR.VBVMR_Input_GetDeviceNumber();
	for (int i = 0; i < num; i++) {
		ret = iVMR.VBVMR_Input_GetDeviceDescA(i, &deviceType, &deviceName[0], &deviceId[0]);
		if (ret == 0) {
			switch (deviceType) {
			case VBVMR_DEVTYPE_MME:
				blog(LOG_INFO, "MME (%i): %s", i, deviceName);
				break;
			case VBVMR_DEVTYPE_WDM:
				blog(LOG_INFO, "WDM (%i): %s", i, deviceName);
				break;
			case VBVMR_DEVTYPE_KS:
				blog(LOG_INFO, "KS (%i): %s", i, deviceName);
				break;
			case VBVMR_DEVTYPE_ASIO:
				blog(LOG_INFO, "ASIO (%i): %s", i, deviceName);
				break;
			default:
				continue;
			}
		}
	}

	long opts = VBVMR_AUDIOCALLBACK_IN | VBVMR_AUDIOCALLBACK_OUT | VBVMR_AUDIOCALLBACK_MAIN;
	char application_name[64] = "obs-voicemeeter";
	ret = iVMR.VBVMR_AudioCallbackRegister(opts, audioCallback, NULL, application_name);
	switch (ret) {
	case 0:
		break;
	case -1:
		blog(LOG_ERROR, "Error %i registering audio callback", ret);
		return false;
	case 1:
		blog(LOG_ERROR, "Error %i registering audio callback: %s has already registered a callback", ret, application_name);
		return false;
	default:
		blog(LOG_ERROR, "Unexpected code %i registering audio callback", ret);
		return false;
	}
	iVMR.VBVMR_AudioCallbackStart();
	
	struct obs_source_info voicemeeter_input_capture = { 0 };
	voicemeeter_input_capture.id = "voicemeeter_input_capture";
	voicemeeter_input_capture.type = OBS_SOURCE_TYPE_INPUT;
	voicemeeter_input_capture.output_flags = OBS_SOURCE_AUDIO;
	voicemeeter_input_capture.create = vi_create;
	voicemeeter_input_capture.destroy = vi_destroy;
	voicemeeter_input_capture.update = vi_update;
	voicemeeter_input_capture.get_defaults = vi_get_defaults;
	voicemeeter_input_capture.get_name = vi_name;
	voicemeeter_input_capture.get_properties = vi_get_properties;

	obs_register_source(&voicemeeter_input_capture);

	return true;
}

void obs_module_unload()
{
	blog(LOG_INFO, "client logging out");
	OBSBufferInsertIn.Disconnect();
	OBSBufferInsertOut.Disconnect();
	OBSBufferMain.Disconnect();

	OBSBufferInsertIn.Clear([](VBVMR_T_AUDIOBUFFER_TS &buf) {
		for (int i = 0; i < buf.data.audiobuffer_nbi; i++) {
			bfree(buf.data.audiobuffer_r[i]);
		}
		for (int i = 0; i < buf.data.audiobuffer_nbo; i++) {
			bfree(buf.data.audiobuffer_w[i]);
		}
	});
	OBSBufferInsertOut.Clear([](VBVMR_T_AUDIOBUFFER_TS &buf) {
		for (int i = 0; i < buf.data.audiobuffer_nbi; i++) {
			bfree(buf.data.audiobuffer_r[i]);
		}
		for (int i = 0; i < buf.data.audiobuffer_nbo; i++) {
			bfree(buf.data.audiobuffer_w[i]);
		}
	});
	/* Need a bit of clarification on how the "Main" buffer's handled */
	OBSBufferMain.Clear([](VBVMR_T_AUDIOBUFFER_TS &buf) {
		for (int i = 0; i < buf.data.audiobuffer_nbi; i++) {
			bfree(buf.data.audiobuffer_r[i]);
		}
		for (int i = 0; i < buf.data.audiobuffer_nbo; i++) {
			bfree(buf.data.audiobuffer_w[i]);
		}
	});

	iVMR.VBVMR_AudioCallbackStop();
	iVMR.VBVMR_AudioCallbackUnregister();
	iVMR.VBVMR_Logout();
}
