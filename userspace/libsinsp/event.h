#pragma once

typedef class sinsp sinsp;
typedef class sinsp_threadinfo sinsp_threadinfo;
typedef class sinsp_fdinfo sinsp_fdinfo;

///////////////////////////////////////////////////////////////////////////////
// Wrapper that exports the libscap event tables
///////////////////////////////////////////////////////////////////////////////
class sinsp_evttables
{
public:
	const struct ppm_event_info* m_event_info;
	const struct ppm_syscall_desc* m_syscall_info_table;
};

///////////////////////////////////////////////////////////////////////////////
// Event parameter wrapper class
///////////////////////////////////////////////////////////////////////////////
class SINSP_PUBLIC sinsp_evt_param
{
public:
	void init(char* valptr, uint16_t len);
	char* m_val;
	uint16_t m_len;
};

///////////////////////////////////////////////////////////////////////////////
// Event class
///////////////////////////////////////////////////////////////////////////////
class SINSP_PUBLIC sinsp_evt
{
public:
	enum param_fmt
	{
		PF_NORMAL,	// Normal screen output
		PF_JSON,	// Json formatting
		PF_SIMPLE,	// Reduced output, e.g. not type character for FDs
	};

	//
	// Event subcategory specialization based on the fd type
	//
	enum subcategory
	{
		SC_UNKNOWN = 0,
		SC_NONE = 1,
		SC_OTHER = 2,
		SC_FILE = 3,
		SC_NET = 4,
		SC_IPC = 5,
	};

	//
	// Information regarding an event category, enriched with fd state
	//
	struct category
	{
		ppm_event_category m_category;	// Event category from the driver
		subcategory m_subcategory;		// Domain for IO and wait events
	};

	sinsp_evt();
	sinsp_evt(sinsp* inspector);
	~sinsp_evt();

	void init();
	void init(uint8_t* evdata, uint16_t cpuid);
	uint64_t get_num();
	int16_t get_cpuid();
	uint16_t get_type();
	ppm_event_flags get_flags();
	uint64_t get_ts();
	const char* get_name();
	event_direction get_direction();
	int64_t get_tid();
	void set_iosize(uint32_t size);
	uint32_t get_iosize();
	sinsp_threadinfo* get_thread_info(bool query_os_if_not_found = false);
	uint32_t get_num_params();
	sinsp_evt_param* get_param(uint32_t id);
	const char* get_param_name(uint32_t id);
	const struct ppm_param_info* get_param_info(uint32_t id);
	const char* get_param_as_str(uint32_t id, OUT const char** resolved_str, param_fmt fmt = PF_NORMAL);
	string get_param_value_str(const char* name, bool resolved = true);
	string get_param_value_str(string& name, bool resolved = true);
	void get_category(OUT sinsp_evt::category* cat);

	static bool compare(ppm_cmp_operator op, ppm_param_type type, void* operand1, void* operand2);

//	int32_t to_str(IN evt_param_info* param, OUT char* str, uint32_t strlen);
private:
	void load_params();
	string get_param_value_str(uint32_t id, bool resolved);

	sinsp* m_inspector;
	scap_evt* m_pevt;
	uint16_t m_cpuid;
	uint64_t m_evtnum;
	bool m_params_loaded;
	const struct ppm_event_info* m_info;
	vector<sinsp_evt_param> m_params;
	char m_paramstr_storage[1024];
	char m_resolved_paramstr_storage[1024];
	sinsp_threadinfo* m_tinfo;
	sinsp_fdinfo* m_fdinfo;
	uint32_t m_iosize;
#ifdef _DEBUG
	bool m_filtered_out;
#endif

	friend class sinsp;
	friend class sinsp_parser;
	friend class sinsp_threadinfo;
	friend class sinsp_analyzer;
};
