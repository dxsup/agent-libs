//
// Why isn't this parser written using antlr or some other parser generator?
// Essentially, after dealing with that stuff multiple times in the past, and fighting for a day
// to configure everything with crappy documentation and code that doesn't compile,
// I decided that I agree with this http://mortoray.com/2012/07/20/why-i-dont-use-a-parser-generator/
// and that I'm going with a manually written parser. The grammar is simple enough that it's not
// going to take more time. On the other hand I will avoid a crappy dependency that breaks my 
// code at every new release, and I will have a cleaner and easier to understand code base.
//

#include "sinsp.h"
#include "sinsp_int.h"

#ifdef HAS_FILTERING
#include "filter.h"
#include "filterchecks.h"

///////////////////////////////////////////////////////////////////////////////
// type-based comparison functions
///////////////////////////////////////////////////////////////////////////////
bool flt_compare_uint64(ppm_cmp_operator op, uint64_t operand1, uint64_t operand2)
{
	switch(op)
	{
	case CO_EQ:
		return (operand1 == operand2);
	case CO_NE:
		return (operand1 != operand2);
	case CO_LT:
		return (operand1 < operand2);
	case CO_LE:
		return (operand1 <= operand2);
	case CO_GT:
		return (operand1 > operand2);
	case CO_GE:
		return (operand1 >= operand2);
	default:
		throw sinsp_exception("'contains' not supported for numeric filters");
		return false;
	}
}

bool flt_compare_int64(ppm_cmp_operator op, int64_t operand1, int64_t operand2)
{
	switch(op)
	{
	case CO_EQ:
		return (operand1 == operand2);
	case CO_NE:
		return (operand1 != operand2);
	case CO_LT:
		return (operand1 < operand2);
	case CO_LE:
		return (operand1 <= operand2);
	case CO_GT:
		return (operand1 > operand2);
	case CO_GE:
		return (operand1 >= operand2);
	default:
		throw sinsp_exception("'contains' not supported for numeric filters");
		return false;
	}
}

bool flt_compare_string(ppm_cmp_operator op, char* operand1, char* operand2)
{
	switch(op)
	{
	case CO_EQ:
		return (strcmp(operand1, operand2) == 0);
	case CO_NE:
		return (strcmp(operand1, operand2) != 0);
	case CO_CONTAINS:
		return (strstr(operand1, operand2) != NULL);
	case CO_LT:
		throw sinsp_exception("'<' not supported for numeric filters");
	case CO_LE:
		throw sinsp_exception("'<=' not supported for numeric filters");
	case CO_GT:
		throw sinsp_exception("'>' not supported for numeric filters");
	case CO_GE:
		throw sinsp_exception("'>=' not supported for numeric filters");
	default:
		ASSERT(false);
		throw sinsp_exception("invalid filter oprator " + std::to_string(op));
		return false;
	}
}

bool flt_compare(ppm_cmp_operator op, ppm_param_type type, void* operand1, void* operand2)
{
	switch(type)
	{
	case PT_INT8:
		return flt_compare_int64(op, (int64_t)*(int8_t*)operand1, (int64_t)*(int8_t*)operand2);
	case PT_INT16:
		return flt_compare_int64(op, (int64_t)*(int16_t*)operand1, (int64_t)*(int16_t*)operand2);
	case PT_INT32:
		return flt_compare_int64(op, (int64_t)*(int32_t*)operand1, (int64_t)*(int32_t*)operand2);
	case PT_INT64:
	case PT_FD:
	case PT_PID:
		return flt_compare_int64(op, *(int64_t*)operand1, *(int64_t*)operand2);
	case PT_UINT8:
	case PT_SIGTYPE:
		return flt_compare_uint64(op, (uint64_t)*(int8_t*)operand1, (uint64_t)*(int8_t*)operand2);
	case PT_UINT16:
	case PT_SYSCALLID:
		return flt_compare_uint64(op, (uint64_t)*(int16_t*)operand1, (uint64_t)*(int16_t*)operand2);
	case PT_UINT32:
		return flt_compare_uint64(op, (uint64_t)*(int32_t*)operand1, (uint64_t)*(int32_t*)operand2);
	case PT_UINT64:
	case PT_RELTIME:
	case PT_ABSTIME:
		return flt_compare_uint64(op, *(uint64_t*)operand1, *(uint64_t*)operand2);
	case PT_CHARBUF:
		return flt_compare_string(op, (char*)operand1, (char*)operand2);
	case PT_BYTEBUF:
	case PT_ERRNO:
	case PT_SOCKADDR:
	case PT_SOCKTUPLE:
	case PT_FDLIST:
	case PT_FSPATH:
	default:
		ASSERT(false);
		return false;
	}
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check implementation
///////////////////////////////////////////////////////////////////////////////
sinsp_filter_check::sinsp_filter_check()
{
	m_boolop = BO_NONE;
	m_cmpop = CO_NONE;
	m_inspector = NULL;
	m_field = NULL;
}

void sinsp_filter_check::set_inspector(sinsp* inspector)
{
	m_inspector = inspector;
}

sinsp_filter_check* sinsp_filter_check::new_filter_check_from_name(string name)
{
	sinsp_filter_check* res;

	//////////////////////////////////////////////////////////////////////////////
	// ADD NEW FILTER CHECK CLASSES HERE
	//////////////////////////////////////////////////////////////////////////////
	if(sinsp_filter_check_fd::recognize_operand(name))
	{
		sinsp_filter_check_fd* chk_fd = new sinsp_filter_check_fd();
		res = (sinsp_filter_check*)chk_fd;
	}
	else if(sinsp_filter_check_thread::recognize_operand(name))
	{
		sinsp_filter_check_thread* chk_thread = new sinsp_filter_check_thread();
		res = (sinsp_filter_check*)chk_thread;
	}
	else if(sinsp_filter_check_event::recognize_operand(name))
	{
		sinsp_filter_check_event* chk_event = new sinsp_filter_check_event();
		res = (sinsp_filter_check*)chk_event;
	}
	else if(sinsp_filter_check_user::recognize_operand(name))
	{
		sinsp_filter_check_user* chk_user = new sinsp_filter_check_user();
		res = (sinsp_filter_check*)chk_user;
	}
	else
	{
		//
		// If you are implementing a new filter check and this point is reached,
		// it's very likely that you've forgotten to add your filter to the list above
		//
		res = NULL;
	}

	return res;
}

char* sinsp_filter_check::rawval_to_string(uint8_t* rawval, const event_field_info* finfo)
{
	char* prfmt;

	switch(finfo->m_type)
	{
		case PT_INT8:
			if(finfo->m_print_format == PF_DEC)
			{
				prfmt = (char*)"%" PRId8;
			}
			else if(finfo->m_print_format == PF_HEX)
			{
				prfmt = (char*)"%" PRIX8;
			}
			else
			{
				ASSERT(false);
			}

			snprintf(m_getpropertystr_storage,
					 sizeof(m_getpropertystr_storage),
					 prfmt, *(int8_t *)rawval);
			return m_getpropertystr_storage;
		case PT_INT16:
			if(finfo->m_print_format == PF_DEC)
			{
				prfmt = (char*)"%" PRId16;
			}
			else if(finfo->m_print_format == PF_HEX)
			{
				prfmt = (char*)"%" PRIX16;
			}
			else
			{
				ASSERT(false);
			}

			snprintf(m_getpropertystr_storage,
					 sizeof(m_getpropertystr_storage),
					 prfmt, *(int16_t *)rawval);
			return m_getpropertystr_storage;
		case PT_INT32:
			if(finfo->m_print_format == PF_DEC)
			{
				prfmt = (char*)"%" PRId32;
			}
			else if(finfo->m_print_format == PF_HEX)
			{
				prfmt = (char*)"%" PRIX32;
			}
			else
			{
				ASSERT(false);
			}

			snprintf(m_getpropertystr_storage,
					 sizeof(m_getpropertystr_storage),
					 prfmt, *(int32_t *)rawval);
			return m_getpropertystr_storage;
		case PT_INT64:
			if(finfo->m_print_format == PF_DEC)
			{
				prfmt = (char*)"%" PRId64;
			}
			else if(finfo->m_print_format == PF_10_PADDED_DEC)
			{
				prfmt = (char*)"%09" PRId64;
			}
			else if(finfo->m_print_format == PF_HEX)
			{
				prfmt = (char*)"%" PRIX64;
			}
			else
			{
				ASSERT(false);
			}

			snprintf(m_getpropertystr_storage,
					 sizeof(m_getpropertystr_storage),
					 prfmt, *(int64_t *)rawval);
			return m_getpropertystr_storage;
		case PT_L4PROTO: // This can be resolved in the future
		case PT_UINT8:
			if(finfo->m_print_format == PF_DEC)
			{
				prfmt = (char*)"%" PRIu8;
			}
			else if(finfo->m_print_format == PF_HEX)
			{
				prfmt = (char*)"%" PRIu8;
			}
			else
			{
				ASSERT(false);
			}

			snprintf(m_getpropertystr_storage,
					 sizeof(m_getpropertystr_storage),
					 prfmt, *(uint8_t *)rawval);
			return m_getpropertystr_storage;
		case PT_PORT: // This can be resolved in the future
		case PT_UINT16:
			if(finfo->m_print_format == PF_DEC)
			{
				prfmt = (char*)"%" PRIu16;
			}
			else if(finfo->m_print_format == PF_HEX)
			{
				prfmt = (char*)"%" PRIu16;
			}
			else
			{
				ASSERT(false);
			}

			snprintf(m_getpropertystr_storage,
					 sizeof(m_getpropertystr_storage),
					 prfmt, *(uint16_t *)rawval);
			return m_getpropertystr_storage;
		case PT_UINT32:
			if(finfo->m_print_format == PF_DEC)
			{
				prfmt = (char*)"%" PRIu32;
			}
			else if(finfo->m_print_format == PF_HEX)
			{
				prfmt = (char*)"%" PRIu32;
			}
			else
			{
				ASSERT(false);
			}

			snprintf(m_getpropertystr_storage,
					 sizeof(m_getpropertystr_storage),
					 prfmt, *(uint32_t *)rawval);
			return m_getpropertystr_storage;
		case PT_UINT64:
		case PT_RELTIME:
		case PT_ABSTIME:
			if(finfo->m_print_format == PF_DEC)
			{
				prfmt = (char*)"%" PRIu64;
			}
			else if(finfo->m_print_format == PF_10_PADDED_DEC)
			{
				prfmt = (char*)"%09" PRIu64;
			}
			else if(finfo->m_print_format == PF_HEX)
			{
				prfmt = (char*)"%" PRIX64;
			}
			else
			{
				ASSERT(false);
			}

			snprintf(m_getpropertystr_storage,
					 sizeof(m_getpropertystr_storage),
					 prfmt, *(uint64_t *)rawval);
			return m_getpropertystr_storage;
		case PT_CHARBUF:
			return (char*)rawval;
		case PT_SOCKADDR:
			ASSERT(false);
			return NULL;
		case PT_SOCKFAMILY:
			ASSERT(false);
			return NULL;
		case PT_BOOL:
			if(*(uint32_t*)rawval == 0)
			{
				return (char*)"true";
			}
			else
			{
				return (char*)"false";
			}
		default:
			ASSERT(false);
			throw sinsp_exception("wrong event type " + to_string(finfo->m_type));
	}
}

char* sinsp_filter_check::tostring(sinsp_evt* evt)
{
	uint8_t* rawval = extract(evt);
	return rawval_to_string(rawval, m_field);
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_expression implementation
///////////////////////////////////////////////////////////////////////////////
sinsp_filter_expression::sinsp_filter_expression()
{
	m_parent = NULL;
}

sinsp_filter_expression::~sinsp_filter_expression()
{
	uint32_t j;

	for(j = 0; j < m_checks.size(); j++)
	{
		delete m_checks[j];
	}
}

void sinsp_filter_expression::add_check(sinsp_filter_check* chk)
{
	m_checks.push_back(chk);
}

void sinsp_filter_expression::parse(string expr)
{
}

bool sinsp_filter_expression::compare(sinsp_evt *evt)
{
	uint32_t j;
	uint32_t size = m_checks.size();
	bool res = true;
	bool chkres;
	 
	for(j = 0; j < size; j++)
	{
		sinsp_filter_check* chk = m_checks[j];
		ASSERT(chk != NULL);

		chkres = chk->compare(evt);
		if(j == 0)
		{
			switch(chk->m_boolop)
			{
			case BO_NONE:
				res = chkres;
				break;
			case BO_NOT:
				res = !chkres;
				break;
			default:
				ASSERT(false);
				break;
			}
		}
		else
		{
			switch(chk->m_boolop)
			{
			case BO_OR:
				res = res || chkres;
				break;
			case BO_AND:
				res = res && chkres;
				break;
			case BO_ORNOT:
				res = res || !chkres;
				break;
			case BO_ANDNOT:
				res = res && !chkres;
				break;
			default:
				ASSERT(false);
				break;
			}
		}
	}

	return res;
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter implementation
///////////////////////////////////////////////////////////////////////////////
sinsp_filter::sinsp_filter(string fltstr, sinsp* inspector)
{
//fltstr = "thread.tid=2162";
//fltstr = "user.name = loris";

	m_inspector = inspector;
	m_scanpos = -1;
	m_scansize = 0;
	m_state = ST_NEED_EXPRESSION;
	m_curexpr = &m_filter;
	m_last_boolop = BO_NONE;
	m_nest_level = 0;

	parse(fltstr);
}

bool sinsp_filter::isblank(char c)
{
	if(c == ' ' || c == '\t' || c == '\n' || c == '\r')
	{
		return true;
	}
	else
	{
		return false;
	}
}

bool sinsp_filter::is_special_char(char c)
{
	if(c == '(' || c == ')' || c == '!' || c == '=' || c == '<' || c == '>')
	{
		return true;
	}

	return false;
}

char sinsp_filter::next()
{
	while(true)
	{
		m_scanpos++;

		if(m_scanpos >= m_scansize)
		{
			return 0;
		}

		if(!isblank(m_fltstr[m_scanpos]))
		{
			return m_fltstr[m_scanpos];
		}
	}
}

string sinsp_filter::next_operand()
{
	int32_t start;

	//
	// Skip spaces
	//
	if(isblank(m_fltstr[m_scanpos]))
	{
		next();
	}

	//
	// Mark the beginning of the word
	//
	start = m_scanpos;

	for(; m_scanpos < m_scansize; m_scanpos++)
	{
		char curchar = m_fltstr[m_scanpos];

		if(isblank(curchar) || is_special_char(curchar))
		{
			//
			// End of word
			//
			ASSERT(m_scanpos > start);
			string res = m_fltstr.substr(start, m_scanpos - start);

			if(curchar == '(' || curchar == ')')
			{
				m_scanpos--;
			}

			return res;
		}
	}

	//
	// End of filter
	//
	return m_fltstr.substr(start, m_scansize - 1);
}

bool sinsp_filter::compare_no_consume(string str)
{
	if(m_scanpos + (int32_t)str.size() >= m_scansize)
	{
		return false;
	}

	string tstr = m_fltstr.substr(m_scanpos, str.size());

	if(tstr == str)
	{
		return true;
	}
	else
	{
		return false;
	}
}

ppm_cmp_operator sinsp_filter::next_comparison_operator()
{
	int32_t start;

	//
	// Skip spaces
	//
	if(isblank(m_fltstr[m_scanpos]))
	{
		next();
	}

	//
	// Mark the beginning of the word
	//
	start = m_scanpos;

	if(compare_no_consume("="))
	{
		m_scanpos += 1;
		return CO_EQ;
	}
	else if(compare_no_consume("!="))
	{
		m_scanpos += 2;
		return CO_NE;
	}
	else if(compare_no_consume("<="))
	{
		m_scanpos += 2;
		return CO_LE;
	}
	else if(compare_no_consume("<"))
	{
		m_scanpos += 1;
		return CO_LT;
	}
	else if(compare_no_consume(">="))
	{
		m_scanpos += 2;
		return CO_GE;
	}
	else if(compare_no_consume(">"))
	{
		m_scanpos += 1;
		return CO_GT;
	}
	else if(compare_no_consume("contains"))
	{
		m_scanpos += 8;
		return CO_CONTAINS;
	}
	else
	{
		throw sinsp_exception("filter error: unrecognized comparison operator after " + m_fltstr.substr(0, start));
	}
}

void sinsp_filter::parse_check(sinsp_filter_expression* parent_expr, boolop op)
{
	uint32_t startpos = m_scanpos;
	string operand1 = next_operand();
	sinsp_filter_check* chk = sinsp_filter_check::new_filter_check_from_name(operand1);

	if(chk == NULL)
	{
		throw sinsp_exception("filter error: unrecognized operand " + 
			operand1 + " at pos " + to_string(startpos));
	}

	chk->set_inspector(m_inspector);

	ppm_cmp_operator co = next_comparison_operator();
	string operand2 = next_operand();

	chk->m_boolop = op;
	chk->m_cmpop = co;
	chk->parse_field_name(operand1.c_str());
	chk->parse_filter_value(operand2.c_str());

	parent_expr->add_check(chk);
}

void sinsp_filter::push_expression(boolop op)
{
	sinsp_filter_expression* newexpr = new sinsp_filter_expression();
	newexpr->m_boolop = op;
	newexpr->m_parent = m_curexpr;
	m_last_boolop = BO_NONE;

	m_curexpr->m_checks.push_back((sinsp_filter_check*)newexpr);
	m_curexpr = newexpr;
	m_nest_level++;
}

void sinsp_filter::pop_expression()
{
	ASSERT(m_curexpr->m_parent != NULL);

	m_curexpr = m_curexpr->m_parent;
	m_nest_level--;
}

void sinsp_filter::parse(string fltstr)
{
	m_fltstr = fltstr;
	m_scansize = m_fltstr.size();

	while(true)
	{
		char a = next();

		switch(a)
		{
		case 0:
			//
			// Finished parsing the filter string
			//
			if(m_nest_level != 0)
			{
				throw sinsp_exception("filter error: unexpected end of filter");
			}

			if(m_state != ST_EXPRESSION_DONE)
			{
				throw sinsp_exception("filter error: unexpected end of filter at position " + to_string(m_scanpos));
			}

			//
			// Good filter
			//
			return;

			break;
		case '(':
			if(m_state != ST_NEED_EXPRESSION)
			{
				throw sinsp_exception("unexpected '(' after " + m_fltstr.substr(0, m_scanpos));
			}

			push_expression(m_last_boolop);

			break;
		case ')':
			pop_expression();
			break;
		case 'o':
			if(next() == 'r')
			{
				m_last_boolop = BO_OR;
			}
			else
			{
				throw sinsp_exception("syntax error in filter at position " + to_string(m_scanpos));
			}

			if(m_state != ST_EXPRESSION_DONE)
			{
				throw sinsp_exception("unexpected 'or' after " + m_fltstr.substr(0, m_scanpos));
			}

			m_state = ST_NEED_EXPRESSION;

			break;
		case 'a':
			if(next() == 'n' && next() == 'd')
			{
				m_last_boolop = BO_AND;
			}
			else
			{
				throw sinsp_exception("syntax error in filter at position " + to_string(m_scanpos));
			}

			if(m_state != ST_EXPRESSION_DONE)
			{
				throw sinsp_exception("unexpected 'and' after " + m_fltstr.substr(0, m_scanpos));
			}

			m_state = ST_NEED_EXPRESSION;

			break;
		case 'n':
			if(next() == 'o' && next() == 't')
			{
				m_last_boolop = (boolop)((uint32_t)m_last_boolop | BO_NOT);
			}
			else
			{
				throw sinsp_exception("syntax error in filter at position " + to_string(m_scanpos));
			}

			if(m_state != ST_EXPRESSION_DONE && m_state != ST_NEED_EXPRESSION)
			{
				throw sinsp_exception("unexpected 'not' after " + m_fltstr.substr(0, m_scanpos));
			}

			m_state = ST_NEED_EXPRESSION;

			break;
		default:
			parse_check(m_curexpr, m_last_boolop);

			m_state = ST_EXPRESSION_DONE;

			break;
		}
	}

	vector<string> components = sinsp_split(m_fltstr, ' ');
}

bool sinsp_filter::run(sinsp_evt *evt)
{
	return m_filter.compare(evt);
}

#endif // HAS_FILTERING
