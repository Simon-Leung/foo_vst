#include "StdAfx.h"
#include "vst.h"
#include "manager.h"
#include "editor.h"
#include "shared.h"

using namespace stringcvt;

class unload_vst : public main_thread_callback {
	vst* m_vst;
public:
	unload_vst(vst* p_vst) : m_vst(p_vst) { }

	void callback_run() override
	{
		if (m_vst != NULL)
		{
			delete m_vst;
		}
	}
};

// Previously a different way of unloading of VSTs was used.
// To prevent them from reloading between tracks, we delayed
// reference count decrement for N ms. This required a separate
// thread (different from the loading thread) which is not very
// good for VSTs. Now there is a session-long VST holder, which
// acquires plug-ins on_init and releases them on_quit, tracking
// changes in the core DSP settings.

static const GUID guid_output_limit =
	{ 0xb845278, 0x89ea, 0x46fc,
	{ 0xbe, 0x6f, 0x6d, 0x11, 0x1f, 0xfe, 0x94, 0x8c } };
static advconfig_integer_factory output_limit(
	"Default output number limit",
	guid_output_limit, guid_adv_vst, 0, 6, 1, 32);

static const GUID guid_declicker_size =
	{ 0xf0048ef8, 0x9ef0, 0x4d7e,
	{ 0xa2, 0x4c, 0x55, 0x63, 0xe0, 0xff, 0xa0, 0x9e } };
static advconfig_integer_factory declicker_size(
	"Declicker size",
	guid_declicker_size, guid_adv_vst, 0, 4, 0, 1000);

// Eats output of VSTs which process zero input on DSP destruction.
class declicker : public initquit {
public:
	void on_init()
	{
		size = (unsigned)declicker_size.get();
		// Previously, there were buffers of dk_smpl in size and all
		// pointers were set to the beginning of these buffers.
		// Apparantly, some plug-ins don't like that (EngineersFilter)
		for (int i = 0; i < dk_ch * dk_ptrs; i++)
		{
			ptrs_i[i] = &(declicker::plain_i[(i % dk_ch) * dk_smpl]);
			ptrs_o[i] = &(declicker::plain_o[(i % dk_ch) * dk_smpl]);
		}
	}
	void on_quit() {}
	void eat_click(AEffect* p_effect)
	{
		for (unsigned i = 0; i < size; i++)
		{
			memset(plain_i, 0, dk_smpl * dk_ch * sizeof(float));
			p_effect->processReplacing(p_effect, ptrs_i, ptrs_o, dk_smpl);
		}
	}
private:
	static const unsigned dk_smpl = 256;
	static const unsigned dk_ch = 8;
	static const unsigned dk_ptrs = 16;
	float plain_i[dk_smpl * dk_ch];
	float plain_o[dk_smpl * dk_ch];
	float* ptrs_i[dk_ch * dk_ptrs];
	float* ptrs_o[dk_ch * dk_ptrs];
	unsigned size;
	// We can handle up to dk_ch × dk_ptrs channels
};

static service_factory_single_t<declicker> g_declicker;

critical_section vst::acquire_release_sync;

vst::vst(const entry & p_entry, bool p_unload) :
		m_module(NULL),
		m_effect(NULL),
		m_editor_opened(false),
		m_ok(false),
		m_ref_count(0),
		entry(p_entry),
		m_srate(0),
		m_channels(0),
		m_ins_num(0),
		m_outs_num(0),
		m_data_size(0),
		m_sample_count(0),
		m_bypass(false),
		m_input(NULL),
		m_output(NULL),
		m_input_ptrs(NULL),
		m_output_ptrs(NULL),
		m_pass_svc_flag_removal(false)
{

	HINSTANCE inst = core_api::get_my_instance();
	m_abspath = p_entry.get_path();
	m_path = m_abspath;
	if (PathIsRelative(string_os_from_utf8(m_abspath)))
	{
		// Convert relative to absolute based on exe dir
		wchar_t exe[MAX_PATH] = {0}, fname[MAX_PATH] = {0};
		GetModuleFileName(NULL, exe, MAX_PATH);
		PathRemoveFileSpec(exe);
		wchar_t out[MAX_PATH] = {0};
		PathCombine(fname, exe, string_os_from_utf8(m_abspath));
		m_abspath = string_utf8_from_os(fname);
	}
	abort_callback_impl aci;
	if (!foobar2000_io::filesystem::g_exists(m_abspath, aci))
	{
		LoadStringRsrc(inst, IDS_VSTNOTFOUND, m_error_msg);
		m_error_msg << "\r\n\r\n" << m_abspath;
		return;
	}
	m_module = LoadLibrary(string_os_from_utf8(m_abspath));
	if (!m_module)
	{
		LoadStringRsrc(inst, IDS_VSTREFUSED, m_error_msg);
		m_error_msg << "\r\n\r\n" << m_abspath;
		return;
	}
	vst_entrypoint vep = reinterpret_cast<vst_entrypoint>(
		GetProcAddress(m_module, "VSTPluginMain"));
	if (!vep) vep = reinterpret_cast<vst_entrypoint>(
		GetProcAddress(m_module, "main"));
	if (!vep)
	{
		LoadStringRsrc(inst, IDS_VSTNOMAIN, m_error_msg);
		m_error_msg << "\r\n\r\n" << m_abspath;
		return;
	}
	m_effect = vep(host_callback);
	if (!m_effect)
	{
		LoadStringRsrc(inst, IDS_VSTNOINST, m_error_msg);
		m_error_msg << "\r\n\r\n" << m_abspath;
		return;
	}
	int v = m_effect->dispatcher(m_effect, effGetVstVersion, 0, 0, NULL, 0.0f);
	if (m_effect->magic != kEffectMagic)
	{
		LoadStringRsrc(inst, IDS_VSTMAGIC, m_error_msg);
		m_error_msg << "\r\n\r\n" << m_abspath;
		return;
	}
	m_effect->resvd2 = reinterpret_cast<VstIntPtr>(this);
	if (m_effect->numInputs == 0 || m_effect->numOutputs == 0)
	{
		LoadStringRsrc(inst, IDS_VSTZEROIO, m_error_msg);
		m_error_msg << "\r\n\r\n" << m_abspath;
		return;
	}
	m_io.reset();
	m_io << m_effect->numInputs << "/" << m_effect->numOutputs;
	int ctgr = m_effect->dispatcher(
		m_effect, effGetPlugCategory, 0, 0, NULL, 0.0f);
	if (ctgr == kPlugCategSynth || ctgr == kPlugCategOfflineProcess)
	{
		LoadStringRsrc(inst, IDS_VSTWRONGCAT, m_error_msg);
		m_error_msg << "\r\n\r\n" << m_abspath;
		return;
	}
	m_has_editor = (m_effect->flags & effFlagsHasEditor) > 0;
	m_effect->dispatcher(m_effect, effOpen, 0, 0, NULL, 0.0f);
	{
		// Product name
		char prod[kVstMaxProductStrLen * 2] = {0}; // x2 just in case
		m_effect->dispatcher(m_effect, effGetProductString, 0, 0, prod, 0.0f);
		m_product = string_utf8_from_ansi(prod);
		if (m_product.length() == 0) m_product = string_filename(m_abspath);
		// Vendor
		char vendor[kVstMaxVendorStrLen * 2] = {0}; // x2 just in case
		m_effect->dispatcher(m_effect, effGetVendorString, 0, 0, vendor, 0.0f);
		m_vendor = string_utf8_from_ansi(vendor);
		if (m_vendor.length() == 0) m_vendor = "n/s";
		m_version = "n/a";
	}
	for (int i = 0; i < 4; i++)
	{
		reinterpret_cast<int*>(&m_guid)[i] = m_effect->uniqueID;
	}
	// Set some program (some plug-ins require that)
	{
		m_effect->dispatcher(m_effect, effBeginSetProgram, 0, 0, NULL, 0.0f);
		m_effect->dispatcher(m_effect, effSetProgram, 0, 0, NULL, 0.0f);
		m_effect->dispatcher(m_effect, effEndSetProgram, 0, 0, NULL, 0.0f);
	}
	// Set up processing stuff
	{
		m_effect->dispatcher(m_effect, effSetProcessPrecision, 0,
			kVstProcessPrecision32, NULL, 0.0f);
		m_effect->dispatcher(m_effect, effStartProcess, 0, 0, NULL, 0.0f);
		m_effect->dispatcher(m_effect, effSetBlockSize, 0, 4096, NULL, 0.0f);
	}
	m_ok = true;
	m_out_limit = min(m_effect->numOutputs,	(unsigned short)output_limit.get());
	if (p_unload) unload();
	// We'll need to choose the thread later
	m_mainthread = core_api::is_main_thread();
	{
		LoadStringRsrc(inst, IDS_VSTNOERRORS, m_error_msg);
		m_error_msg << "\r\n\r\n" << m_abspath;
	}
}

vst::~vst(void)
{
	unload();
	delete_buffers();
}

void vst::unload()
{
	if (m_effect != NULL && m_ok)
	{
		m_effect->dispatcher(m_effect, effStopProcess, 0, 0, NULL, 0.0f);
		m_effect->dispatcher(m_effect, effClose, 0, 0, NULL, 0.0f);
		m_effect = NULL;
	}
	if (m_module != NULL)
	{
		FreeLibrary(m_module);
		m_module = NULL;
	}
}

// see playback_flag description in the bottom
static bool playing_back = false;

// Simple reference-counting management of VST instances
vst* vst::g_acquire_vst(const dsp_preset & p_preset,
						const entry & p_entry, owner_id oid)
{
	// Owner flags were introduced after the bugreport concerning
	// processing multiple streams in one instance.
	// For example, if svc flag is set, then another svc won't access
	// this instance.
	// The rule isn't applicable to instance guard, which is permitted to
	// acquire the same vst several times.
	// -------
	acquire_release_sync.enter();
	vst* result = NULL;
	// Prevent “convert” entries from being linked to core instances.
	if (!(oid == vst::owner_dspsvc && !playing_back))
	{
		for (const_iterator<vst*> iter = instanceList().first();
			iter.is_valid(); iter.next())
		{
			vst* iv = *iter;
			if (
				// Only compare the random ID we add to every dsp_preset
				memcmp(iv->m_preset.get_data(), p_preset.get_data(), 4) == 0 &&
				(!iv->m_owners.have_item(oid) || oid == vst::owner_instguard))
			{
				result = *iter;
				break;
			}
		}
	}
	if (result == NULL)
	{
		result = new vst(p_entry);
		if (!result->m_ok)
		{
			popup_message::g_complain(result->get_msg());
		}
		result->set_preset(p_preset);
	}
	result->m_ref_count++;
	result->m_owners.add_item(oid);
	acquire_release_sync.leave();
	return result;
}

vst* vst::g_release_vst(vst* p_instance, owner_id oid)
{
	acquire_release_sync.enter();
	if (!(p_instance->m_pass_svc_flag_removal && oid == owner_dspsvc))
	{
		p_instance->m_owners.remove_item(oid);
	}
	else
	{
		p_instance->m_pass_svc_flag_removal = false;
	}
	vst* result = NULL;
	if (--p_instance->m_ref_count == 0)
	{
		if (p_instance->m_mainthread && !core_api::is_main_thread())
		{
			// :TODO: possible sync failure
			service_impl_t<unload_vst>* cb =
			new service_impl_t<unload_vst>(p_instance);
			static_api_ptr_t<main_thread_callback_manager>()->
				add_callback(cb);
		}
		else
		{
			delete p_instance;
		}
	}
	else
	{
		result = p_instance;
	}
	acquire_release_sync.leave();
	return result;
}

void vst::delete_buffers()
{
	if (m_input != NULL) delete[] m_input;
	if (m_output != NULL) delete[] m_output;
	if (m_input_ptrs != NULL) delete[] m_input_ptrs;
	if (m_output_ptrs != NULL) delete[] m_output_ptrs;
}

void vst::suspend()
{
	if (!m_ok) return;
	m_effect->dispatcher(m_effect, effMainsChanged, 0, 0, NULL, 0.0f);
}

void vst::resume()
{
	// Resuming in a bypassed state isn't allowed
	if (!m_ok || m_bypass) return;
	m_effect->dispatcher(m_effect, effMainsChanged, 0, 1, NULL, 0.0f);
}

void vst::set_bypass(bool bypass)
{
	if (m_bypass != bypass && m_bypass == false) // Bypass: off → on
	{
		suspend();
	}
	else if (m_bypass != bypass && m_bypass == true) // Bypass: on → off
	{
		resume();
	}
	m_bypass = bypass;
}

bool vst::process_chunk(audio_chunk * chunk, abort_callback &)
{
	// Deinterleaving stuff could be done in-place but it's not worth it
	if (!m_ok || m_bypass) return true;
	
	preset_sync.enter();
	if (m_srate != chunk->get_sample_rate())
	{
		suspend();
		m_srate = chunk->get_sample_rate();
		m_effect->dispatcher(m_effect, effSetSampleRate, 0, 0, NULL,
			static_cast<float>(m_srate));
		resume();
	}
	// Block size has grown or needs to be set.
	// Less CPU usage if chunk data size isn't stable.
	// Doesn't occur often as opposed to simple “!=” check.
	if (m_channels < chunk->get_channels() ||
		m_data_size < chunk->get_data_size() ||
		m_sample_count < chunk->get_sample_count() ||
		m_ins_num < (unsigned)m_effect->numInputs ||
		m_outs_num < (unsigned)m_effect->numOutputs)
	{
		m_channels = chunk->get_channels();
		m_data_size = chunk->get_data_size();
		m_sample_count = chunk->get_sample_count();
		m_ins_num = (unsigned)m_effect->numInputs;
		m_outs_num = (unsigned)m_effect->numOutputs;
		delete_buffers();
		m_input = new float[m_sample_count * m_ins_num];
		m_output = new float[m_sample_count * m_outs_num];
		m_input_ptrs = new float*[m_ins_num];
		m_output_ptrs = new float*[m_outs_num];
		suspend();
		m_effect->dispatcher(
			m_effect, effSetBlockSize, 0, m_sample_count, NULL, 0.0f);
		resume();
	}
	// Newly allocated memory contains trash. Gotta mute it.
	if (m_ins_num != m_channels)
	{
		memset(m_input, 0, m_sample_count * m_ins_num * sizeof(float));
	}
	if (m_outs_num != m_channels)
	{
		memset(m_output, 0, m_sample_count * m_outs_num * sizeof(float));
	}
	// Deinterleave the data from the chunk to the m_input
	for (unsigned
		c = 0,
		n = min(chunk->get_channels(), (unsigned)m_effect->numInputs);
		c < n; c++)
	{
		audio_sample* data = chunk->get_data();
		unsigned chunk_channels = chunk->get_channels();
		for (unsigned s = 0, b = chunk->get_sample_count(); s < b; s++)
		{
			m_input[s + b * c] = data[s * chunk_channels + c];
		}
	}
	// Set ptrs
	for (unsigned c = 0; c < (unsigned)m_effect->numInputs; c++)
	{
		m_input_ptrs[c] = &m_input[chunk->get_sample_count() * c];
	}
	for (unsigned c = 0; c < (unsigned)m_effect->numOutputs; c++)
	{
		m_output_ptrs[c] = &m_output[chunk->get_sample_count() * c];
	}
	// Process
	m_effect->processReplacing(
		m_effect, m_input_ptrs, m_output_ptrs, chunk->get_sample_count());
	
	// Update channel config
	int lim = m_out_limit;
	// Auto
	if (lim < 1) lim = min((int)chunk->get_channels(), m_effect->numOutputs);
	
	chunk->set_channels(lim);
	chunk->grow_data_size(chunk->get_sample_count() * lim);
	// Interleave the output data back to the chunk
	for (int c = 0; c < lim; c++)
	{
		audio_sample* data = chunk->get_data();
		for (unsigned s = 0, b = chunk->get_sample_count(); s < b; s++)
		{
			data[s * lim + c] = m_output[s + b * c];
		}
	}
	preset_sync.leave();
	return true;
}

void vst::get_preset(const GUID & p_owner, dsp_preset & out)
{
	preset_sync.enter();
	dsp_preset_builder builder;
	// As mentioned in dsp_vst, we have to add random data when
	// asked for default dsp_preset to be able to identify instances later
	// It's not a panacea though. I had 
	srand(GetTickCount());
	builder << rand();
	// Previously, I saved banks instead of programs. My bad.
	// So now I ought to fix that without breaking compatibility.
	// I add marker which is to indicate that it's a program, not a bank.
	builder.write_int('prg!');
	if (m_ok)
	{
		if (m_effect->flags & effFlagsProgramChunks)
		{
			char* data;
			t_size data_size =
				m_effect->dispatcher(m_effect, effGetChunk, 1, 0, &data, 0.0f);
			pfc::array_staticsize_t<char> chunk;
			chunk.set_data_fromptr(data, data_size);
			builder.write_byte_block(chunk);
		}
		else
		{
			pfc::array_staticsize_t<float> params(m_effect->numParams);
			for (int i = 0, j = m_effect->numParams; i < j; i++)
			{
				params[i] = m_effect->getParameter(m_effect, i);
			}
			builder.write_array(params);
		}
	}
	builder.write_int(m_out_limit);
	builder.write_int(m_bypass ? 1 : 0);
	builder.finish(p_owner, out);
	preset_sync.leave();
}

void vst::set_preset(const dsp_preset & in)
{
	upd_preset(in);
	if (!m_ok) return;
	preset_sync.enter();
	static const short rndsize = 8;
	const dsp_preset * p = &in;
	if (in.get_data_size() > rndsize)
	{
		int hdr;
		dsp_preset_parser parser(in);
		parser.skip(4);
		parser.read_int(hdr);
		if (hdr != 'prg!')
		{
			// Old format, no prg! (!grp) header
			parser.reset();
			parser.skip(4);
		}
		if ((m_effect->flags & effFlagsProgramChunks) > 0)
		{
			array_t<char> chunk;
			parser.read_byte_block(chunk);
			BOOL isprogram = hdr == 'prg!' ? TRUE : FALSE;
			char* d = chunk.get_ptr();
			t_size s = chunk.get_size();
			////////////
			// Intermediate format (only used internally in 0.8.1.0)
			if (*((int*)d) == 'prg!')
			{
				d = d + 4;
				s -= 4;
				isprogram = TRUE;
			}
			////////////
			m_effect->dispatcher(m_effect, effSetChunk, isprogram, s, d, 0.0f);
		}
		else
		{
			array_t<float> params;
			parser.read_array(params);
			float* data = params.get_ptr();
			for (int i = 0, j = m_effect->numParams; i < j; i++)
			{
				m_effect->setParameter(m_effect, i, *(data + i));
			}
		}
		if (parser.get_remaining() >= 4)
		{
			parser.read_int(m_out_limit);
		}
		if (parser.get_remaining() >= 4)
		{
			int bypass_int = 0;
			parser.read_int(bypass_int);
			m_bypass = (bypass_int == 1);
		}
	}
	preset_sync.leave();
}

// When we don't need to load it
void vst::upd_preset(const dsp_preset & in)
{
	m_preset.copy(in);
}

void vst::declick()
{
	if (!m_ok) return;
	g_declicker.get_static_instance().eat_click(m_effect);
}

void vst::load_fxp(const char* p_path)
{
	preset_sync.enter();
	VstInt32 chunk_magic = 0;
	VstInt32 byte_size = 0;
	VstInt32 fx_magic = 0;
	VstInt32 version = 0;
	VstInt32 fx_id = 0;
	VstInt32 fx_version = 0;
	VstInt32 num_params = 0;
	char prg_name[28] = {0};

	using namespace foobar2000_io;
	abort_callback_dummy cb;
	if (!filesystem::g_exists(p_path, cb))
	{
		preset_sync.leave(); return;
	}
	service_ptr_t<file> fxp;
	filesystem::g_open_read(fxp, p_path, cb);
	t_filesize filesize = fxp->get_size(cb);
	if (filesize < 4 || filesize == filesize_invalid)
	{
		popup_message::g_complain(get_rsrc_string(IDS_INVALIDFXP));
		preset_sync.leave(); return;
	}
	fxp->read_bendian_t(chunk_magic, cb);
	if (chunk_magic != 'CcnK')
	{
		popup_message::g_complain(get_rsrc_string(IDS_INVALIDFXP));
		preset_sync.leave(); return;
	}
	fxp->read_bendian_t(byte_size, cb);
	if (filesize < byte_size + 8 || filesize == filesize_invalid)
	{
		popup_message::g_complain(get_rsrc_string(IDS_FXPWRONGSIZE));
		preset_sync.leave(); return;
	}
	fxp->read_bendian_t(fx_magic, cb);
	fxp->read_bendian_t(version, cb);
	if (version != 1)
	{
		popup_message::g_complain(get_rsrc_string(IDS_FXPVERSION));
		preset_sync.leave(); return;
	}
	fxp->read_bendian_t(fx_id, cb);
	// Actually checked by the plug-in
	/*if(fx_id != m_effect->uniqueID) 
	{ 
		popup_message::g_show(get_rsrc_string(IDS_FXPWRONGID),
			get_rsrc_string(IDS_ERROR), popup_message::icon_error);
		preset_sync.leave(); return;
	}*/
	fxp->read_bendian_t(fx_version, cb);
	if (fx_version != m_effect->version)
	{
		if (uMessageBox(NULL,
			get_rsrc_string(IDS_EFFECTVERSION), "Warning", MB_YESNO) != IDYES)
		{
			preset_sync.leave(); return;
		}
	}
	fxp->read_bendian_t(num_params, cb);
	fxp->read(prg_name, 28, cb);
	prg_name[kVstMaxProgNameLen] = 0;
	VstPatchChunkInfo chunk_info = {0};
	{
		chunk_info.version = 1;
		chunk_info.numElements = num_params;
		chunk_info.pluginUniqueID = fx_id;
		chunk_info.pluginVersion = version;
	}
	int canload = m_effect->dispatcher(
		m_effect, effBeginLoadProgram, 0, 0, &chunk_info, 0.0f);
	if (canload == -1)
	{
		popup_message::g_complain(get_rsrc_string(IDS_FXPCANTLOAD));
		preset_sync.leave(); return;
	}
	m_effect->dispatcher(m_effect, effSetProgramName, 0, 0, prg_name, 0.0f);
	if (fx_magic == 'FxCk')
	{
		for (int i = 0; i < num_params; i++)
		{
			float val = 0.0f;
			fxp->read_bendian_t(val, cb);
			m_effect->setParameter(m_effect, i, val);
		}
	}
	else if (fx_magic == 'FPCh')
	{
		VstInt32 chunk_size = 0;
		fxp->read_bendian_t(chunk_size, cb);
		void* data = malloc(chunk_size);
		fxp->read(data, chunk_size, cb);
		m_effect->dispatcher(m_effect, effSetChunk, 1, chunk_size, data, 0.0f);
		free(data);
	}
	else
	{
		popup_message::g_complain(get_rsrc_string(IDS_FXPUNKNOWNCHUNK));
		preset_sync.leave(); return;
	}
	preset_sync.leave(); return;
}

void vst::store_fxp(const char* p_path, const string_base * p_custom_name)
{
	preset_sync.enter();
	char prg_name[28] = {0};
	if (p_custom_name == NULL)
	{
		m_effect->dispatcher(m_effect, effGetProgramName, 0, 0, prg_name, 0.0f);
	}
	else
	{
		stringcvt::string_ansi_from_utf8 ansiname(*p_custom_name);
		const char* str = ansiname.get_ptr();
		strcpy_s(prg_name, 27, str);
	}
	const bool chunky = (m_effect->flags & effFlagsProgramChunks) != 0;
	VstInt32 chunk_size = 0;
	char* data = NULL;
	if (chunky)
	{
		chunk_size =
			m_effect->dispatcher(m_effect, effGetChunk, 1, 0, &data, false);
	}
	else
	{
		chunk_size = m_effect->numParams * sizeof(float);
		data = new char[chunk_size];
		float* arr = reinterpret_cast<float*>(data);
		for (int i = 0, j = m_effect->numParams; i < j; i++)
		{
			float val = m_effect->getParameter(m_effect, i);
			byte_order::order_native_to_be_t(val);
			arr[i] = val;
		}
	}
	using namespace foobar2000_io;
	service_ptr_t<file> fxp;
	abort_callback_dummy cb;
	filesystem::g_open_write_new(fxp, p_path, cb);
	//
	fxp->write_bendian_t('CcnK', cb);
	fxp->write_bendian_t(chunk_size + (chunky ? sizeof(VstInt32) : 0) + 48, cb);
	fxp->write_bendian_t(chunky ? 'FPCh' : 'FxCk', cb);
	fxp->write_bendian_t(1, cb);
	fxp->write_bendian_t(m_effect->uniqueID, cb);
	fxp->write_bendian_t(m_effect->version, cb);
	fxp->write_bendian_t(m_effect->numParams, cb);
	fxp->write(prg_name, 28, cb);
	if (chunky) fxp->write_bendian_t(chunk_size, cb);
	fxp->write(data, chunk_size, cb);
	//
	if (!chunky) delete[] data;
	preset_sync.leave();
}

VstIntPtr VSTCALLBACK vst::host_callback(
	AEffect* effect, VstInt32 opcode, VstInt32 index,
	VstIntPtr value, void* ptr, float opt)
{
	static list_t<string8> caps;
	vst* _this = NULL;
	if (effect != NULL) _this = reinterpret_cast<vst*>(effect->resvd2);
	VstIntPtr result = 0;
	editor* e = NULL;
	if (effect != NULL) e = reinterpret_cast<editor*>(effect->resvd1);
	switch (opcode)
	{
		case audioMasterVersion:
			result = kVstVersion;
			break;
		case audioMasterAutomate:
			if (e != NULL)
			{
				e->preset_edited();
			}
			break;
		case audioMasterSizeWindow:
			if (e != NULL)
			{
				e->resize_editor(index, value);
			}
			break;
		case audioMasterIdle:
			if (effect != NULL)
			{
				effect->dispatcher(effect, effEditIdle, 0, 0, 0, 0);
			}
			break;
		case audioMasterProcessEvents:
			result = 1;
			break;
		case audioMasterGetVendorString:
			strcpy((char*)ptr, "Yegor Petrov");
			result = 1;
			break;
		case audioMasterGetProductString:
			strcpy((char*)ptr, "Foobar2000 VST 2.4 adapter");
			result = 1;
			break;
		case audioMasterGetAutomationState:
			result = kVstAutomationOff;
			break;
		case audioMasterGetVendorVersion:
			result = version_int; // stdafx
			break;
		case audioMasterCanDo:
			if (caps.get_count() == 0)
			{
				caps += "supplyIdle";
				caps += "sizeWindow";
			}
			result = caps.have_item(string_utf8_from_ansi((char*)ptr)) ? 1 : 0;
			break;
		case audioMasterGetLanguage:
			result = kVstLangEnglish;
			break;
		case audioMasterCurrentId:
			if (effect != NULL)
			{
				result = effect->uniqueID;
			}
			break;
		case audioMasterGetDirectory:
			{
				wchar_t dll[MAX_PATH] = {0};
				string_os_from_utf8 path(_this->m_abspath);
				const wchar_t* d = path.get_ptr();
				wcscpy(dll, d);
				PathRemoveFileSpec(dll);
				result = (VstIntPtr)(string_ansi_from_wide(dll).get_ptr());
			}
			break;
		default:
			result = 0;
			break;
	}

	return result;
}

// Declared as friend in vst.h
// :TRICKY:
	// Since there is a moment when two SVCs need to own same VST,
	// we have to erase SVC flags here.
	// This also introduces another problem. The older service releases
	// its instance of VST and thus the flag is erased. But nothing is
	// going to set it since the instance has already been acquired.
	// So we need another flag which is to indicate that one removal must
	// be passed.
void erase_svc_flag(vst* p_vst)
{
	p_vst->m_owners.remove_item(vst::owner_dspsvc);
	p_vst->m_pass_svc_flag_removal = true;
}

// Holds core chain VSTs in memory during the session.
// Helps to keep loading and unloading in the same thread.
class instance_guard : public dsp_config_callback {
	list_t<vst*> m_instances;
	int m_count;
	critical_section sync;
public:
	void on_init()
	{
		dsp_chain_config_impl chain;
		static_api_ptr_t<dsp_config_manager>()->get_core_settings(chain);
		fill_list(m_instances, chain);
		//m_count = chain.get_count();
	}

	void on_quit()
	{
		for (int i = 0, j = m_instances.get_count(); i < j; i++)
		{
			m_instances[i] =
				vst::g_release_vst(m_instances[i], vst::owner_instguard);
		}
	}

	void on_core_settings_change(const dsp_chain_config & p_newdata)
	{
		sync.enter();
		for (int i = 0, j = m_instances.get_count(); i < j; i++)
		{
			erase_svc_flag(m_instances[i]);
		}
		//if (m_count == p_newdata.get_count()) return;
		list_t<vst*> newlist;
		fill_list(newlist, p_newdata);
		for (int i = 0, j = m_instances.get_count(); i < j; i++)
		{
			/*m_instances[i] =*/
				vst::g_release_vst(m_instances[i], vst::owner_instguard);
		}
		//m_instances.remove_item(NULL);
		m_instances.remove_all();
		m_instances.add_items<list_t<vst*>>(newlist);
		//m_count = p_newdata.get_count();
		sync.leave();
	}

	// :TRICKY:
	// Of course, it won't work if someone edits GUIDs manually.
	// It's fast though. Let's keep it this way.
	static bool is_vst(const GUID & p_guid)
	{
		const int* g1 = reinterpret_cast<const int*>(&p_guid);
		const int* g2 = g1 + 1;
		const int* g3 = g1 + 2;
		const int* g4 = g1 + 3;
		return (*g1 == *g2) && (*g1 == *g3) && (*g1 == *g4);
	}

private:
	void fill_list(list_t<vst*> & p_list,
		const dsp_chain_config & p_listdata)
	{
		for (t_size i = 0, j = p_listdata.get_count(); i < j; i++)
		{
			const dsp_preset & p = p_listdata.get_item(i);
			const GUID g = p.get_owner();
			if (is_vst(g))
			{
				const entry* e = manager::get_vst_entry(g);
				if (e != NULL)
				{
					p_list.add_item(
						vst::g_acquire_vst(p, *e, vst::owner_instguard));
				}
			}
		}
	}
};
static service_factory_single_t<instance_guard> g_instguard;

// Just triggers instance_guard “events”
class dsp_init : public initquit
{
	void on_init()
	{
		g_instguard.get_static_instance().on_init();
	}
	void on_quit()
	{
		g_instguard.get_static_instance().on_quit();
	}
};
static service_factory_single_t<dsp_init> g_dspinit;

// This one is needed for the case when the same preset used to “convert” as
// the one used in the core chain. It sets flag indicating whether playback is
// going on. If such flag isn't set then the acqusition method will return a
// new instance of a VST plug-in.
class playback_flag : public play_callback_static
{
	unsigned get_flags() { return flag_on_playback_starting | flag_on_playback_stop; }
	
	void on_playback_starting(play_control::t_track_command p_command,bool p_paused)
	{
		playing_back = true;
	}

	void on_playback_stop(play_control::t_stop_reason p_reason)
	{
		playing_back = false;
	}
	
	void on_playback_new_track(metadb_handle_ptr p_track) {}
	void on_playback_seek(double p_time) {}
	void on_playback_pause(bool p_state) {}
	void on_playback_edited(metadb_handle_ptr p_track) {}
	void on_playback_dynamic_info(const file_info & p_info) {}
	void on_playback_dynamic_info_track(const file_info & p_info) {}
	void on_playback_time(double p_time) {}
	void on_volume_change(float p_new_val) {}
};

static play_callback_static_factory_t<playback_flag> g_pln;