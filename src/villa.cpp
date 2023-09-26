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
#include "baresip/modules/aufile/aufile.h"

void play_stop_handler(struct play *play, void *arg);

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
		end = _atoms.size();
	}

	for (int i = start; i < end; ++i) {

		const Atom& a = _atoms[i];

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

	if (_mode & m_loop) {
		position %= length();
	}

	for (auto a = _atoms.begin(); a != _atoms.end(); ++a) {

		if (std::holds_alternative<Play>(*a)) {
			l += std::get<Play>(*a).length();
		}
		else if (std::holds_alternative<DTMF>(*a)) {
			l += std::get<DTMF>(*a).length();
		}
		else if (std::holds_alternative<Record>(*a)) {
			l += std::get<Record>(*a).length();
		}

		if (l >= position) {
			if (_mode & m_mute) {

				size_t offset = (position - l_prev);

				_current = a;

				if (std::holds_alternative<Play>(*a)) {
					std::get<Play>(*a).set_offset(offset);
				}
				else if (std::holds_alternative<DTMF>(*a)) {
					std::get<DTMF>(*a).set_offset(offset);
				}

				return;
			}
		}

		l_prev = l;
	}
}

std::string Molecule::desc() const {
	std::string desc = std::to_string(_priority) + ' ' + mode_string(_mode);

	for (auto a: _atoms) {
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
	for (auto i = _molecules[m->_priority].begin();
		i != _molecules[m->_priority].end(); ++i) {

		if (m == &(*i)) {
			_molecules[m->_priority].erase(i);
			break;
		}
	}
}

std::vector<Molecule>::iterator VQueue::next() {

	for (int p = max_priority; p >= 0; --p) {

		for (auto i = _molecules[p].begin();
			i != _molecules[p].end(); ++i) {

			if (i->_atoms.size()) {
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
		if (stopped->_mode & m_discard) {
			discard(stopped);
		}
		else if (n != end() && stopped == &(*n)) {
			// The current molecule was stopped
			++n->_current;
		}
	}

	if (n != end()) {

		if (n->_mode & m_pause) {
			n->set_position(n->_position);
		}
		else if (n->_time_stopped) {
			size_t pos = now - n->_time_stopped;

			if (n->_mode & m_mute) {

				if (pos >= n->length()) {
					if (n->_mode & m_mute) {
						discard(&(*n));
						n = next();
					}
					else if (n->_mode & m_loop) {
						pos = pos % n->length();
					}

					n->set_position(pos);
				}
			}
		}

		if (n->_current != n->end()) {

			Atom &a = *n->_current;

			if (std::holds_alternative<Play>(a)) {

				Play& play = std::get<Play>(a);

				int err = play_file_ext(&play._play, _session->_player,
					play.filename().c_str(), 0, "default", _session->_id.c_str(),
					play.offset());
				if (err) {
					DEBUG_PRINTF("play %s failed: %s\n", play.filename().c_str(), strerror(err));
					n->_current = n->_atoms.erase(n->_current);
					return err;
				}
				else {
					DEBUG_INFO("playing %s\n", play.filename().c_str());
				}
				play_set_finish_handler(play._play, play_stop_handler, (void*)&(*n));
			}
			else if (std::holds_alternative<DTMF>(a)) {

				DTMF& d = std::get<DTMF>(a);

				if (++d > d.size()) {
					d.reset();
					++n->_current;
					if (n->_current != n->_atoms.end()) {
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

				int err = play_file_ext(&d._play, baresip_player(), filename.c_str(), 0,
						"default", _session->_id.c_str(), 0);
				if (err) {
					return err;
				}
				play_set_finish_handler(d._play, play_stop_handler, &(*n));
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

				int err = ausrc->alloch(&record._rec, ausrc,
					&sprm, nullptr, nullptr, nullptr, nullptr);

				if (err) {
					return err;
				}
			}
		}
	}

	if (n != end()) {
		n->_time_started = tmr_jiffies();
	}

	return 0;
}

int VQueue::enqueue(const Molecule& m) {

	auto stopped = next();

	_molecules[m._priority].push_back(m);

	if (stopped == end()) {
		return schedule(nullptr);
	}

	if (stopped->_current != stopped->end()) {
		Atom &a = *stopped->_current;

		if (std::holds_alternative<Play>(a)) {
			std::get<Play>(a).stop();
		}
	}
	return schedule(&(*stopped));
}


void play_stop_handler(struct play *play, void *arg) {

	DEBUG_INFO("play file previous\n");
	size_t now = tmr_jiffies();

	Molecule* stopped = (Molecule*)arg;
	assert(stopped);

	stopped->_time_stopped = now;
	stopped->_position = now - stopped->_time_started;

	assert(stopped->_session);
	VQueue &queue = stopped->_session->_queue;

	queue.schedule(stopped);
}

Session::Session(struct call *call, struct json_tcp *jt) : _call(call), _jt(jt), _queue(this) {
	_id = call_id(call);
}

void Session::dtmf(char key) {

	odict *od;
	odict_alloc(&od, DICT_BSIZE);

	odict_entry_add(od, "event", ODICT_BOOL, true);

	if (key == '\x04') { // end-of-transmission
		auto now = std::chrono::system_clock::now();

		auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - _dtmf_start);
		odict_entry_add(od, "type", ODICT_STRING, "dtmf_end");
		odict_entry_add(od, "key", ODICT_STRING, _dtmf.c_str());
		odict_entry_add(od, "duration", ODICT_INT, duration);
		_dtmf.erase();

	}
	else {
		char k[2] = { key, 0 };
		_dtmf = k;
		odict_entry_add(od, "type", ODICT_STRING, "dtmf_begin");
		odict_entry_add(od, "key", ODICT_STRING, _dtmf.c_str());

		_dtmf_start = std::chrono::system_clock::now();
	}

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

	_player = (struct player*)mem_deref(_player);
}

Session::~Session() {
	hangup();
}

std::unordered_map<std::string, Session> Sessions;
std::vector<ua*> UserAgents;
std::unordered_map<std::string,call*> PendingCalls;

odict *create_response(const char* type, const char* token, int result)
{
	odict *od = nullptr;
	odict_alloc(&od, DICT_BSIZE);

	odict_entry_add(od, "type", ODICT_STRING, type);
	odict_entry_add(od, "class", ODICT_STRING, "villa");
	odict_entry_add(od, "response", ODICT_BOOL, true);
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

		session->dtmf(key);
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

	size_t optional_offset(const odict* entry) {

		size_t offset = 0;
		const odict_entry *eo = odict_lookup(entry, "offset");
		if (eo) {
			if (odict_entry_type(eo) != ODICT_INT) {
				DEBUG_PRINTF("villa: command enqueue: optional offset has invalid type");
				return 0;
			}
			offset = odict_entry_int(eo);
		}

		return offset;
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
				const auto [it, _] = Sessions.insert(std::make_pair(cid, Session(call, jt)));

				Session* session = &it->second;
				call_set_handlers(call, villa_call_event_handler,
		       		villa_dtmf_handler, session);

				play_init(&session->_player);
				play_set_path(session->_player, "/usr/local/share/baresip");

				audio_set_devicename(call_audio(call), cid.c_str(), cid.c_str());
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
				sit->second.hangup(scode, reason);

				Sessions.erase(sit);
			}
			else {
				auto cit = PendingCalls.find(cid);
				if (cit != PendingCalls.end()) {
					call_hangup(cit->second, scode, reason);
				}
				else {
					create_response("hangup", token, EINVAL);
				}
			}

			return create_response("hangup", token, 0);
		}
		else if (strcmp(command, "enqueue") == 0) {

			struct le *le = parms->lst.head;
			if (!le) {
				DEBUG_PRINTF("villa: command enqueue: missing parameters");
				return nullptr;
			}

			const odict_entry *e = (const odict_entry*)le->data;
			if (odict_entry_type(e) != ODICT_STRING) {
				DEBUG_PRINTF("villa: command enqueue: parameter 1 (call_id) invalid type");
				return nullptr;
			}

			const char *call_id = odict_entry_str(e);
			auto sit = Sessions.find(call_id);
			if (sit == Sessions.end()) {
				DEBUG_PRINTF("villa: command enqueue: session not found");
				return nullptr;
			}

			Session& session = sit->second;

			le = le->next;
			if (!le) {
				DEBUG_PRINTF("villa: command enqueue: parameter 2 (priority) missing");
				return nullptr;
			}

			e = (const odict_entry*)le->data;
			if (odict_entry_type(e) != ODICT_INT) {
				DEBUG_PRINTF("villa: command enqueue: parameter 2 (priority) invalid type");
				return nullptr;
			}

			Molecule m(&session);
			m._priority = odict_entry_int(e);

			le = le->next;
			if (!le) {
				DEBUG_PRINTF("villa: command enqueue: parameter 3 (mode) missing");
				return nullptr;
			}

			e = (const odict_entry*)le->data;
			if (odict_entry_type(e) != ODICT_INT) {
				DEBUG_PRINTF("villa: command enqueue: parameter 3 (mode) invalid type");
				return nullptr;
			}

			m._mode = (mode)odict_entry_int(e);

			int count = 4;
			for (le = le->next; le; le = le->next, ++count) {

				e = (const odict_entry*)le->data;
				if (odict_entry_type(e) != ODICT_OBJECT) {
					DEBUG_PRINTF("villa: command enqueue: parameter %d (atom) invalid type", count);
					return nullptr;
				}

				struct odict *atom = odict_entry_object(e);
				std::string type(odict_string(atom, "type"));

				if (type == "play") {
					const char* filename = odict_string(atom, "filename");
					if (!filename) {
						DEBUG_PRINTF("villa: command enqueue: parameter %d (atom) invalid type", count);
						return nullptr;
					}

					Play play(filename);
					size_t offset = optional_offset(atom);
					if (offset) {
						play.set_offset(offset);
					}

					m.push_back(play);
				}
				else if (type == "record") {
					const char* filename = odict_string(atom, "filename");
					if (!filename) {
						DEBUG_PRINTF("villa: command enqueue: parameter %d (atom) filename has invalid type", count);
						return nullptr;
					}

					Record record(filename);
					m.push_back(record);
				}
				else if (type == "dtmf") {
					const char* digits = odict_string(atom, "digits");
					if (!digits) {
						DEBUG_PRINTF("villa: command enqueue: parameter %d (atom) digits has invalid type", count);
						return nullptr;
					}

					DTMF dtmf(digits);
					size_t offset = optional_offset(atom);
					if (offset) {
						dtmf.set_offset(offset);
					}
					m.push_back(dtmf);
				}
			}

			session._queue.enqueue(m);

			return create_response("enqueue", token, 0);
		}

		return nullptr;
	}

	int villa_status(struct re_printf *pf, void *arg)
	{
		return 0;
	}
}