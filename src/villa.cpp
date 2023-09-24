/**
 * @file src/villa.cpp Villa priority queue logic and call handling
 *
 * Copyright (C) 2023 Lars Immisch
 */

#define DEBUG_MODULE "villa"
#define DEBUG_LEVEL 7

#include <stddef.h>
#include <re.h>
#include <baresip.h>
#include <re_dbg.h>

#include "villa.h"
#include "json_tcp.h"
#include "villa_module.h"
#include "baresip/modules/aufile/aufile.h"

void play_stop_handler(struct play *play, void *arg);

//-------------------------------------------------------------------------

static ausrc_st* g_rec;
static struct play *g_play;

static VQueue vqueue;

//-------------------------------------------------------------------------

std::string mode_string(mode m) {

	std::string modestr;

	for (int e = m_last; e != 0; e >>= 1) {

		if (modestr.size() && modestr.back() != '|') {
			modestr += "|";
		}

		mode em = (mode)(m & e);

		switch (em) {
			case m_discard:
				modestr += "discard";
				break;
			case m_pause:
				modestr += "pause";
				break;
			case m_mute:
				modestr += "mute";
				break;
			case m_restart:
				modestr += "restart";
				break;
			case m_dont_interrupt:
				modestr += "dont_interrupt";
				break;
			case m_loop:
				modestr += "loop";
				break;
		}
	}

	if (modestr.size() && modestr.back() == '|') {
		modestr.pop_back();
	}

	return modestr;
}

bool is_atom_start(const std::string &token) {
	if (token[0] == 'p' || token[0] == 'r' || token[0] == 'd') {
		return true;
	}

	return false;
}

size_t Play::set_filename(const std::string& filename) {

	struct aufile* au;
	struct aufile_prm prm;
	struct config_audio *cfg = &conf_config()->audio;

	_filename = filename;

	std::string path(cfg->audio_path);
	if (path.back() != '/') {
		path += "/";
	}
	path += filename;

	int err = aufile_open(&au, &prm, path.c_str(), AUFILE_READ);
	if (err) {
		return 0;
	}

	size_t length = aufile_get_length(au, &prm);

	mem_deref(au);

	return length;
}

// @pragma mark Molecule

size_t Molecule::length(int start, int end) const {
	size_t l = 0;

	if (end < 0) {
		end = atoms.size();
	}

	for (int i = start; i < end; ++i) {

		const Atom& a = atoms[i];

		if (std::holds_alternative<Play>(a)) {
			l += std::get<Play>(a).length();
		}
		else if (std::holds_alternative<DTMF>(a)) {
			l += std::get<DTMF>(a).length();
		}
		if (std::holds_alternative<Record>(a)) {
			l += std::get<Record>(a).length();
		}
	}

	return l;
}

void Molecule::set_position(size_t position) {

	// search the correct atom/offset to select
	size_t l = 0;
	size_t l_prev = 0;

	if (mode & m_loop) {
		position %= length();
	}

	for (size_t i = 0; i < atoms.size(); ++i) {

		Atom& a = atoms[i];

		if (std::holds_alternative<Play>(a)) {
			l += std::get<Play>(a).length();
		}
		else if (std::holds_alternative<DTMF>(a)) {
			l += std::get<DTMF>(a).length();
		}
		else if (std::holds_alternative<Record>(a)) {
			l += std::get<Record>(a).length();
		}

		if (l >= position) {
			if (mode & m_mute) {

				size_t offset = (position - l_prev);

				current = i;

				if (std::holds_alternative<Play>(a)) {
					std::get<Play>(a).set_offset(offset);
				}
				else if (std::holds_alternative<DTMF>(a)) {
					std::get<DTMF>(a).set_offset(offset);
				}

				return;
			}
		}

		l_prev = l;
	}
}

std::string Molecule::desc() const {
	std::string desc = std::to_string(priority) + ' ' + mode_string(mode);

	for (auto a: atoms) {
		if (std::holds_alternative<Play>(a)) {
			const Play& p = std::get<Play>(a);
			desc += " play " + p.filename();
		}
		else if (std::holds_alternative<DTMF>(a)) {
			const DTMF& d = std::get<DTMF>(a);
			desc += " dtmf " + d.dtmf();
		}
		else if (std::holds_alternative<Record>(a)) {
			const Record& r = std::get<Record>(a);
			desc += " record " + r.filename();
		}
	}

	return desc;
}

// @pragma mark VQueue

void VQueue::discard(Molecule* m) {
	for (auto i = molecules[m->priority].begin();
		i != molecules[m->priority].end(); ++i) {

		if (m == &(*i)) {
			molecules[m->priority].erase(i);
			break;
		}
	}
}

std::vector<Molecule>::iterator VQueue::next() {

	for (int p = max_priority; p >= 0; --p) {

		for (std::vector<Molecule>::iterator i = molecules[p].begin(); i != molecules[p].end(); ++i) {

			if (i->atoms.size()) {
				return i;
			}
		}
	}

	return end();
}

int VQueue::schedule(Molecule* stopped) {

	struct config *cfg = conf_config();
	size_t now = tmr_jiffies();

	std::vector<Molecule>::iterator n = next();

	if (stopped) {
		// Just remove Molecules with m_discard
		if (stopped->mode & m_discard) {
			discard(stopped);
			n = next();
		}
		else if (n != end() && stopped == &(*n)) {
			// The current molecule was stopped
			++n->current;
			if (n->current >= n->atoms.size()) {
				discard(stopped);
				n = next();
			}
		}
	}

	while (n != end()) {

		if (n->mode & m_pause) {
			n->set_position(n->position);
		}
		else {
			size_t pos = now - n->time_stopped;

			if (n->mode & m_mute) {

				if (pos >= n->length()) {
					if (n->mode & m_mute) {
						discard(&(*n));
						n = next();
					}
					else if (n->mode & m_loop) {
						pos = pos % n->length();
					}

					n->set_position(pos);
					break;
				}
			}
		}

		Atom a = n->atoms[n->current];

		if (std::holds_alternative<Play>(a)) {

			const Play& play = std::get<Play>(a);

			DEBUG_INFO("playing %s\n", play.filename().c_str());

			// mute
			// audio_mute(audio, true);

			int err = play_file_ext(&g_play, baresip_player(), play.filename().c_str(), 0,
				cfg->audio.alert_mod, cfg->audio.alert_dev,
				play.offset());
			if (err) {
				return err;
			}
			play_set_finish_handler(g_play, play_stop_handler, (void*)&(*n));
		}
		else if (std::holds_alternative<DTMF>(a)) {

			DTMF& d = std::get<DTMF>(a);

			if (++d > d.size()) {
				d.reset();
				n->current++;
				if (n->current >= n->atoms.size()) {
					return schedule(&(*n));
				}
			}

			std:: string filename = "sound";
			if (d.current() == '*') {
				filename += "star.wav";
			}
			else if (d.current() == '#') {
				filename += "route.wav";
			}
			else {
				filename.append((char)tolower(d.current()), 1);
				filename += ".wav";
			}

			DEBUG_INFO("DTMF playing %s\n", filename.c_str());

			// mute
			// audio_mute(audio, true);

			int err = play_file_ext(&g_play, baresip_player(), filename.c_str(), 0,
					cfg->audio.alert_mod, cfg->audio.alert_dev, 0);
			if (err) {
				return err;
			}
			play_set_finish_handler(g_play, play_stop_handler, &(*n));
		}
		else if (std::holds_alternative<Record>(a)) {

			// unmute
			// audio_mute(audio, false);

			Record& record = std::get<Record>(a);

			uint32_t srate = 0;
			uint32_t channels = 0;

			conf_get_u32(conf_cur(), "file_srate", &srate);
			conf_get_u32(conf_cur(), "file_channels", &channels);

			if (!srate) {
				srate = 16000;
			}

			if (!channels) {
				channels = 1;
			}

			ausrc_prm sprm;

			sprm.ch = channels;
			sprm.srate = srate;
			sprm.ptime = PTIME;
			sprm.fmt = AUFMT_S16LE;

			DEBUG_INFO("recording %s\n", record.filename().c_str());

			const struct ausrc *ausrc = ausrc_find(baresip_ausrcl(), "aufile");

			int err = ausrc->alloch(&g_rec, ausrc,
				&sprm, nullptr, nullptr, nullptr, nullptr);

			if (err) {
				return err;
			}
		}

		n = vqueue.next();
	}

	if (n != end() && n->current == 0) {
		n->time_started = tmr_jiffies();
	}

	return 0;
}

int VQueue::enqueue(const Molecule& m, void* arg) {

	vqueue.molecules[m.priority].push_back(m);

	/* Stop the current player or recorder, if any */
	g_play = (struct play*)mem_deref(g_play);
	g_rec = (struct ausrc_st*)mem_deref(g_rec);

	auto stopped = next();
	if (stopped != end()) {
		return schedule(&(*stopped));
	}

	return schedule(nullptr);
}

int VQueue::enqueue(const char* mdesc, void* arg) {

	std::string smdesc(mdesc);
	std::regex ws("\\s+");

	std::sregex_token_iterator iter(smdesc.begin(), smdesc.end(), ws, -1);
	std::sregex_token_iterator end;

	std::vector<std::string> vec(iter, end);
	std::vector<std::string> tokens;

	for (auto a: vec)
	{
		std::smatch m;
		if (!std::regex_match(a, m, ws)) {
			tokens.push_back(a);
		}
	}

	Molecule m;

	auto token = tokens.begin();
	if (token == tokens.end()) {
		perror("missing priority");
		return EINVAL;
	}

	try {
		m.priority = std::stol(*token);
	}
	catch(std::exception&) {
		perror("invalid priority");
		return EINVAL;
	}

	++token;

	if (token == tokens.end()) {
		perror("missing mode");
		return 0;
	}

	m.mode = (mode)0;

	for (int i = 0; i < 2; ++i) {

		if (*token == "loop") {
			m.mode = (mode)(m.mode | m_loop);
		}
		else if (*token == "mute") {
			m.mode = (mode)(m.mode | m_mute);
		}
		else if (*token == "discard") {
			m.mode = (mode)(m.mode | m_discard);
		}
		else if (*token == "pause") {
			m.mode = (mode)(m.mode | m_pause);
		}
		else if (*token == "restart") {
			m.mode = (mode)(m.mode | m_restart);
		}
		else if (*token == "dont_interrupt") {
			m.mode = (mode)(m.mode | m_dont_interrupt);
		}
		else {
			break;
		}
		++token;

		if (token == tokens.end()) {
			perror("missing mode");
			return 0;
		}
	}

	DEBUG_INFO("adding Molecule priority: %d, mode: %s\n", m.priority, mode_string(m.mode).c_str());

	while (token != tokens.end()) {

		if (*token == "p" || *token == "play") {

			++token;
			if (token != tokens.end()) {
				Play play;
				play.set_filename(*token);

				++token;
				if (token != tokens.end()) {

					if (!is_atom_start(*token)) {
						play.set_offset(std::stol(*token));
					}
					else {
						++token;
					}
				}

				m.atoms.push_back(play);
			}
			else {
				perror("No filename after play atom");
				return 0;
			}
		}
		else if (*token == "r" || *token == "record") {
			++token;
			if (token != tokens.end()) {
				Record record;
				record.set_filename(*token);

				++token;
				if (token != tokens.end()) {

					if (!is_atom_start(*token)) {
						record.set_max_silence(std::stol(*token));
						++token;

					}
				}

				m.atoms.push_back(record);
			}
			else {
				perror("No filename after record atom");
				return 0;
			}
		}
		else if (*token == "d" || *token == "dtmf") {
			++token;
			if (token != tokens.end()) {
				DTMF dtmf;
				dtmf.set_dtmf(*token);
				++token;

				if (!is_atom_start(*token)) {
					dtmf.set_inter_digit_delay(std::stol(*token));
					++token;
				}

				m.atoms.push_back(dtmf);
			}
			else {
				perror("No digits after atom_dtmf");
				return 0;
			}
		}
	}

	if (m.atoms.size() == 0) {
		perror("No atom in molecule");
		return 0;
	}

	return enqueue(m, arg);
}


void play_stop_handler(struct play *play, void *arg) {

	DEBUG_INFO("play file previous\n");
	size_t now = tmr_jiffies();

	/* Stop the current player or recorder, if any */
	g_play = (struct play*)mem_deref(g_play);
	g_rec = (struct ausrc_st*)mem_deref(g_rec);

	Molecule* stopped = (Molecule*)arg;

	stopped->time_stopped = now;
	stopped->position = now - stopped->time_started;

	vqueue.schedule((Molecule*)arg);
}

Session::Session(struct call *call, struct json_tcp *jt) : _call(call), _jt(jt) {
	_id = call_id(call);
}

void Session::dtmf(char key) {

	std::string skey(key, 1);
	odict *od;
	odict_alloc(&od, DICT_BSIZE);

	odict_entry_add(od, "event", ODICT_BOOL, true);
	odict_entry_add(od, "type", ODICT_STRING, "dtmf");
	odict_entry_add(od, "key", ODICT_STRING, skey.c_str());

	json_tcp_send(_jt, od);
}

void Session::hangup(int16_t scode, const char* reason) {
	if (_call) {
		call_hangup(_call, scode , reason);
		_call = nullptr;

		odict *od;
		odict_alloc(&od, DICT_BSIZE);

		odict_entry_add(od, "event", ODICT_BOOL, true);
		odict_entry_add(od, "type", ODICT_STRING, "call_closed");
		odict_entry_add(od, "status_code", ODICT_INT, scode);
		odict_entry_add(od, "reason", ODICT_STRING, reason);

		json_tcp_send(_jt, od);
	}
}

Session::~Session() {
	hangup();
}

std::unordered_map<std::string, Session> Sessions;
std::vector<ua*> UserAgents;
std::unordered_map<std::string,call*> PendingCalls;

odict *create_response(const char* command, const char* token, int result)
{
	odict *od = nullptr;
	odict_alloc(&od, DICT_BSIZE);

	odict_entry_add(od, "type", ODICT_STRING, "response");
	odict_entry_add(od, "class", ODICT_STRING, "villa");
	odict_entry_add(od, "command", ODICT_STRING, command);
	if (token) {
		odict_entry_add(od, "token", ODICT_STRING, token);
	}
	odict_entry_add(od, "result", ODICT_INT, result);

	return od;
}

extern "C" {
	void villa_call_event_handler(struct call *call, enum call_event ev,
			    const char *str, void *arg)
	{
		Session *session = (Session*)arg;

		switch (ev) {
			case CALL_EVENT_CLOSED:
			{
				std::string cid(call_id(call));

				auto sit = Sessions.find(cid);
				if (sit != Sessions.end()) {

					DEBUG_INFO("villa: %s CALL_CLOSED\n", session->_id.c_str());

					session->hangup(200, str);
					Sessions.erase(sit);
				}
				else {
					auto cit = PendingCalls.find(cid);
					if (cit != PendingCalls.end()) {
						DEBUG_PRINTF("villa: %s CALL_CLOSED before accepted: %s\n",
							session->_id.c_str(), str);
						PendingCalls.erase(cit);
						return;
					}
					else {
						DEBUG_INFO("villa: %s CALL_CLOSED, but no session found\n",
							session->_id.c_str());
					}
				}
			}
			break;
		default:
			break;
		}

		DEBUG_PRINTF("villa: received call event: '%d'\n", ev);
	}

	void villa_dtmf_handler(struct call *call, char key, void *arg)
	{
		Session *session = (Session*)arg;

		DEBUG_PRINTF("villa: received DTMF event: key = '%c'\n", key ? key : '.');
	}

	void villa_event_handler(struct ua *ua, enum ua_event ev,
		struct call *call, const char *prm, void *arg)
	{
		switch (ev) {

		case UA_EVENT_CALL_INCOMING:
		{
			std::string cid(call_id(call));
			DEBUG_PRINTF("villa: CALL_INCOMING(%s): peer=%s --> local=%s\n",
				cid.c_str(), call_peeruri(call), call_localuri(call));
			PendingCalls.insert(std::make_pair(cid, call));
		}
			break;
		default:
			break;
		}
	}

	struct odict *villa_command_handler(const char* command,
		struct odict *parms, const char* token, struct json_tcp *jt)
	{
		if (strcmp(command, "listen") == 0) {

			struct le *le = parms->lst.head;
			if (!le) {
				DEBUG_PRINTF("villa: command listen without parameter");
				return nullptr;
			}

			const odict_entry *e = (const odict_entry*)le->data;
			if (odict_entry_type(e) != ODICT_STRING) {
				DEBUG_PRINTF("villa: command listen parameter invalid type");
				return nullptr;
			}

			const char* addr = odict_entry_str(e);

			struct ua *agent;

			int err = ua_alloc(&agent, addr);
			if (!err) {
				UserAgents.push_back(agent);
			}

			return create_response("listen", token, err);
		}
		else if (strcmp(command, "answer") == 0) {

			struct le *le = parms->lst.head;
			if (!le) {
				DEBUG_PRINTF("villa: command accept without parameter");
				return nullptr;
			}

			const odict_entry *e = (const odict_entry*)le->data;
			if (odict_entry_type(e) != ODICT_STRING) {
				DEBUG_PRINTF("villa: command accept parameter invalid type");
				return nullptr;
			}

			std::string cid(odict_entry_str(e));

			auto cit = PendingCalls.find(cid);
			if (cit == PendingCalls.end()) {
				return create_response("answer", token, EINVAL);
			}

			call *call = cit->second;

			int err = call_answer(cit->second, 200, VIDMODE_OFF);

			if (!err) {
				// Create the session
				auto [it, _] = Sessions.insert(std::make_pair(cid, std::move(Session(call, jt))));

				Session* session = &it->second;
				call_set_handlers(call, villa_call_event_handler,
		       		villa_dtmf_handler, session);
			}

			PendingCalls.erase(cit);

			return create_response("answer", token, err);
		}
		else if (strcmp(command, "hangup") == 0) {

			struct le *le = parms->lst.head;
			if (!le) {
				DEBUG_PRINTF("villa: command hangup without parameter");
				return nullptr;
			}

			const odict_entry *e = (const odict_entry*)le->data;
			if (odict_entry_type(e) != ODICT_STRING) {
				DEBUG_PRINTF("villa: command hangup parameter invalid type");
				return nullptr;
			}

			const char* cid = odict_entry_str(e);

			int16_t scode = 200;
			const char* reason = "Bye";

			le = le->next;
			if (le) {
				const odict_entry *e = (const odict_entry*)le->data;
				if (odict_entry_type(e) != ODICT_INT) {
					DEBUG_PRINTF("villa: command hangup parameter 2 invalid type");
					return nullptr;
				}
				scode = odict_entry_int(e);

				le = le->next;
				if (le) {
					e = (const odict_entry*)le->data;
					if (odict_entry_type(e) != ODICT_STRING) {
						DEBUG_PRINTF("villa: command hangup parameter 3 invalid type");
						return nullptr;
					}
					reason = odict_entry_str(e);
				}
			}

			auto sit = Sessions.find(cid);
			if (sit != Sessions.end()) {
				sit->second.hangup();

				call_hangup(sit->second._call, scode, reason);

				Sessions.erase(sit);
			}
			else {

				auto cit = PendingCalls.find(cid);
				if (cit != PendingCalls.end()) {
				}

				int err = call_answer(cit->second, 200, VIDMODE_OFF);
			}

			return create_response("hangup", token, 0);
		}
	}

	int villa_status(struct re_printf *pf, void *arg)
	{
		return 0;
	}
}