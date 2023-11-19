/**
 * @file src/villa.cpp Villa priority queue logic and call handling
 *
 * Copyright (C) 2023 Lars Immisch
 */

#define DEBUG_MODULE "villa"
#define DEBUG_LEVEL 7

#include <stddef.h>
#include <sstream>
#include <re.h>
#include <baresip.h>
#include <re_dbg.h>
#include <stdexcept>

#include "villa.h"
#include "json_tcp.h"

//-------------------------------------------------------------------------

const char *call_event_name(call_event ev) {
	switch (ev) {
		case CALL_EVENT_INCOMING:
			return "CALL_EVENT_INCOMING";
		case CALL_EVENT_OUTGOING:
			return "CALL_EVENT_OUTGOING";
		case CALL_EVENT_RINGING:
			return "CALL_EVENT_RINGING";
		case CALL_EVENT_PROGRESS:
			return "CALL_EVENT_PROGRESS";
		case CALL_EVENT_ANSWERED:
			return "CALL_EVENT_ANSWERED";
		case CALL_EVENT_ESTABLISHED:
			return "CALL_EVENT_ESTABLISHED";
		case CALL_EVENT_CLOSED:
			return "CALL_EVENT_CLOSED";
		case CALL_EVENT_TRANSFER:
			return "CALL_EVENT_TRANSFER";
		case CALL_EVENT_TRANSFER_FAILED:
			return "CALL_EVENT_TRANSFER_FAILED";
		case CALL_EVENT_MENC:
			return "CALL_EVENT_MENC";
		default:
			return "unknown call event";
	}
}

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
		end = _atoms.size() + end + 1;
	}
	else {
		end = std::max(end, (int)_atoms.size());
	}

	for (int i = start; i < end; ++i) {
		l += _atoms[i]->length();
	}

	return l;
}

void Molecule::set_position(size_t position) {

	// search the correct atom/offset to select
	size_t l = 0;
	size_t l_prev = 0;

	for (size_t i = 0; i < _atoms.size(); ++i) {

		AudioOpPtr a = _atoms[i];
		l += a->length();

		if (l >= position) {
			size_t offset = (position - l_prev);

			_current = i;
			a->set_offset(offset);

			return;
		}

		l_prev = l;
	}
}

std::string Molecule::desc() const {
	std::string desc = std::to_string(_priority) + ' ' + mode_string(_mode);

	for (auto &a: _atoms) {
		desc += a->desc();
	}

	return desc;
}

AudioOpPtr &Molecule::current() {

	if (_current >= _atoms.size()) {
		throw std::out_of_range("no current atom");
	}

	return _atoms[_current];
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

int Play::start() {

	_audio = call_audio(_session->_call);
	_stopped = false;

	int err = audio_set_source_offset(_audio, "aufile", _filename.c_str(), _offset);
	if (err) {
		_audio = nullptr;
	}

	return err;
}

void Play::stop()
{
	if (_audio) {
		audio_set_source(_audio, nullptr, nullptr);
		_audio = nullptr;
		_stopped = true;
	}
}

size_t Play::length() const
{
	if (_length) {
		return _length;
	}

	struct aufile *f;
	struct aufile_prm p;

	int err = aufile_open(&f, &p, _filename.c_str(), AUFILE_READ);
	if (err < 0) {
		return 0;
	}

	return aufile_get_length(f, &p);
}

void record_timer(void *arg) {

	Record::TimerId *timer = (Record::TimerId*)arg;

	DEBUG_PRINTF("%s recording %s stopped. Reason: %s\n",
		timer->record->_session->_id.c_str(),
		timer->record->filename().c_str(),
		timer->timer_id == Record::timer_max_silence ? "max silence" : "max length");

	timer->record->stop();
	timer->record->_session->_queue.schedule(VQueue::sched_end_of_file);
}

int Record::start() {

	_audio = call_audio(_session->_call);
	_stopped = false;

	int err = audio_set_player(_audio, "aufile", _filename.c_str());
	if (err) {
		_audio = nullptr;
		return err;
	}

	if (_max_length > 0) {
		tmr_start(&_tmr_max_length, _max_length, record_timer, &_timer_max_length_id);
	}

	if (_max_silence > 0) {
		tmr_start(&_tmr_max_silence, _max_silence, record_timer, &_timer_max_silence_id);
	}

	return err;
}

void Record::stop() {

	if (_audio) {
		_stopped = true;

		tmr_cancel(&_tmr_max_length);
		tmr_cancel(&_tmr_max_silence);

		audio_set_player(_audio, nullptr, nullptr);

		_audio = nullptr;
	}
}

void Record::event_vad(Session*, bool vad) {
	if (vad && _max_silence > 0) {
		tmr_continue(&_tmr_max_silence, _max_silence, record_timer, &_timer_max_silence_id);
	}
}

void Record::event_dtmf(Session *session, char, bool end)
{
	if (!end && _dtmf_stop) {
		DEBUG_PRINTF("%s recording %s stopped. Reason: dtmf\n", session->_id.c_str(), _filename.c_str());

		stop();
		session->_queue.schedule(VQueue::sched_end_of_file);
	}
}


int VQueue::schedule(reason r) {

	size_t now = tmr_jiffies();

	auto current = next();
	if (current == end()) {
		_active = nullptr;
		return 0;
	}

	if (_active) {

		// Just remove Molecules with m_discard that are interrupted
		if (_active->_mode & m_discard && _active != &*current) {
			_active = nullptr;
			_session->molecule_done(*_active);
			discard(_active);
		}
		else if (_active == &(*current)) {
			// The current molecule was stopped or played to the end

			if (r == sched_end_of_file) {
				++current->_current;
				if (!current->is_active()) {
					current->_time_stopped = now;
				}
			}
			else {
				current->_time_stopped = now;
				current->set_position(now - current->_time_started);
			}
		}
	}

	if (current->_mode & m_loop) {

		if (current->_mode & m_restart && !current->is_active()) {
			current->set_position(0);
		}
		else if (current->_mode & m_mute) {
			size_t length = current->length();
			size_t pos = current->_time_started ? (now - current->_time_started) % length : 0;
			DEBUG_PRINTF("setting position to %d\n", pos);
			current->set_position(pos);
		}
		else if (current->_mode & m_pause) {
			size_t pos = current->_time_stopped ? (now - current->_time_stopped) % current->length() : 0;
			current->set_position(pos);
		}
	}

	if (current->is_active()) {

		AudioOpPtr &a = current->current();

		int err = a->start();
		if (err) {
			DEBUG_PRINTF("%s failed: %s\n", a->desc().c_str(), strerror(err));
			current->_atoms.erase(current->_atoms.begin() + current->_current);
			current->_current++;
			return err;
		}

		if (current->_current == 0) {
			current->_time_started = now;
		}
		_active = &(*current);
		DEBUG_INFO("%s started\n", a->desc().c_str());
	}
	else {
		_active = nullptr;
		_molecules[current->_priority].erase(current);
		_session->molecule_done(*current);

		return schedule(r);
	}

	return 0;
}

int VQueue::enqueue(const Molecule& m) {
	_molecules[m._priority].push_back(m);

	if (!_active || _active->_priority < m._priority) {
		if (_active) {
			size_t now = tmr_jiffies();

			_active->_time_stopped = now;
		}

		return schedule(sched_interrupt);
	}

	return 0;
}

Session::Session(struct call *call, struct json_tcp *jt) : _call(call), _jt(jt), _queue(this) {
	_id = call_id(call);
}

void Session::molecule_done(const Molecule& m) const {

	if (!m._id.empty()) {
		odict *od = nullptr;
		odict_alloc(&od, DICT_BSIZE);

		odict_entry_add(od, "event", ODICT_BOOL, true);
		odict_entry_add(od, "type", ODICT_STRING, "molecule_done");
		odict_entry_add(od, "id", ODICT_STRING, m._id.c_str());

		json_tcp_send(_jt, od);
	}
}

void Session::dtmf(char key) {

	odict *od;
	odict_alloc(&od, DICT_BSIZE);

	odict_entry_add(od, "event", ODICT_BOOL, true);
	odict_entry_add(od, "id", ODICT_STRING, _id.c_str());

	if (key != '\x04') {
		char k[2] = { key, 0 };
		_dtmf = k;
		odict_entry_add(od, "type", ODICT_STRING, "dtmf_begin");
		odict_entry_add(od, "key", ODICT_STRING, _dtmf.c_str());

		_dtmf_start = std::chrono::system_clock::now();
	}

	Molecule *active = _queue._active;
	if (active && active->is_active()) {
		active->current()->event_dtmf(this,_dtmf[0], key == '\x04');
	}

	if (key == '\x04') { // end-of-transmission
		auto now = std::chrono::system_clock::now();

		auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - _dtmf_start);
		odict_entry_add(od, "type", ODICT_STRING, "dtmf_end");
		odict_entry_add(od, "key", ODICT_STRING, _dtmf.c_str());
		odict_entry_add(od, "duration", ODICT_INT, duration);
		_dtmf.erase();
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
		odict_entry_add(od, "id", ODICT_STRING, _id.c_str());

		json_tcp_send(_jt, od);
	}
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

	void villa_tcp_disconnected()
	{
		// Lost connection to world, hangup all calls
		for (auto &[id, session] : Sessions) {
			session.hangup(500, "Connection to world lost");
		}

		Sessions.clear();
	}

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

					DEBUG_INFO("%s CALL_CLOSED\n", session->_id.c_str());

					session->hangup(200, str);
					Sessions.erase(sit);
				}
				else {
					auto cit = PendingCalls.find(cid);
					if (cit != PendingCalls.end()) {
						warning("%s CALL_CLOSED before accepted: %s\n",
							session->_id.c_str(), str);
						PendingCalls.erase(cit);
						return;
					}
					else {
						DEBUG_INFO("%s CALL_CLOSED, but no session found\n",
							session->_id.c_str());
					}
				}
			}
			break;
		default:
			break;
		}

		DEBUG_PRINTF("received call event: %s\n", call_event_name(ev));
	}

	void villa_dtmf_handler(struct call *call, char key, void *arg)
	{
		(void)call;

		Session *session = (Session*)arg;

		DEBUG_PRINTF("%s received DTMF event: key = '%c'\n", session->_id.c_str(), key ? key : '.');

		session->dtmf(key);
	}

	void villa_event_handler(struct ua *ua, enum ua_event ev,
		struct call *call, const char *prm, void *arg)
	{
		struct json_tcp *jt = (json_tcp*)arg;
		(void)ua;
		bool send_event = false;

		switch (ev) {

		case UA_EVENT_CALL_INCOMING:
		{
			std::string cid(call_id(call));
			DEBUG_PRINTF("%s: CALL_INCOMING: peer=%s --> local=%s\n",
				cid.c_str(), call_peeruri(call), call_localuri(call));
			PendingCalls.insert(std::make_pair(cid, call));
			send_event = true;
			break;
		}
		case UA_EVENT_END_OF_FILE:
		{
			std::string cid(call_id(call));
			DEBUG_PRINTF("%s END_OF_FILE\n", cid.c_str());

			size_t now = tmr_jiffies();

			auto session = Sessions.find(cid);
			if (session == Sessions.end()) {
				warning("%s END_OF_FILE: no session found\n", cid.c_str());
				return;
			}

			if (session->second._queue._active) {
				Molecule *stopped = session->second._queue._active;
				stopped->_time_stopped = now;

				session->second._queue.schedule(VQueue::sched_end_of_file);
			}
			else {
				warning("villa: no molecule active, but UA_EVENT_END_OF_FILE received");
			}

			break;
		}
		case UA_EVENT_MODULE:
		{
			std::string cid(call_id(call));
			DEBUG_PRINTF("%s MODULE %s\n", cid.c_str(), prm);

			auto session = Sessions.find(cid);
			if (session == Sessions.end()) {
				DEBUG_PRINTF("%s MODULE: no session found\n", cid.c_str());
				return;
			}

			std::stringstream ss(prm);
    		std::string elem;
			std::vector<std::string> elems;

		    while(std::getline(ss, elem, ',')) {
        		elems.push_back(elem);
    		}

			if (elems.size() >= 3) {
				if (elems[0] == "fvad" && elems[1] == "vad_rx") {
					bool vad = elems[2] == "on";

					Molecule *active = session->second._queue._active;
					if (active && active->is_active()) {
						active->current()->event_vad(&session->second, vad);
					}
				}
			}
			else {
				warning("unknown event format '%s\n", prm);
			}
			break;
		}
		default:
			warning("unhandled event %s\n", uag_event_str(ev));
			break;
		}

		if (send_event) {

			struct odict *od = NULL;

			int err = odict_alloc(&od, DICT_BSIZE);
			if (err)
				return;

			err = odict_entry_add(od, "event", ODICT_BOOL, true);
			err |= event_encode_dict(od, ua, ev, call, prm);
			if (err) {
				DEBUG_WARNING("villa: failed to encode event (%m)\n", err);
				return;
			}

			json_tcp_send(jt, od);
		}
	}

	size_t optional_offset(const odict* entry) {

		size_t offset = 0;
		const odict_entry *eo = odict_lookup(entry, "offset");
		if (eo) {
			if (odict_entry_type(eo) != ODICT_INT) {
				warning("command enqueue: optional offset has invalid type");
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
				warning("command listen without parameter");
				return nullptr;
			}

			const odict_entry *e = (const odict_entry*)le->data;
			if (odict_entry_type(e) != ODICT_STRING) {
				warning("command listen parameter invalid type");
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
				warning("command accept without parameter");
				return nullptr;
			}

			const odict_entry *e = (const odict_entry*)le->data;
			if (odict_entry_type(e) != ODICT_STRING) {
				warning("command accept parameter invalid type");
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
			}

			PendingCalls.erase(cit);

			return create_response("answer", token, err);
		}
		else if (strcmp(command, "hangup") == 0) {

			struct le *le = parms->lst.head;
			if (!le) {
				warning("command hangup without parameter\n");
				return nullptr;
			}

			const odict_entry *e = (const odict_entry*)le->data;
			if (odict_entry_type(e) != ODICT_STRING) {
				warning("command hangup parameter invalid type\n");
				return nullptr;
			}

			const char* cid = odict_entry_str(e);

			int16_t scode = 200;
			const char* reason = "Bye";

			le = le->next;
			if (le) {
				const odict_entry *e = (const odict_entry*)le->data;
				if (odict_entry_type(e) != ODICT_INT) {
					warning("command hangup parameter 2 invalid type\n");
					return nullptr;
				}
				scode = odict_entry_int(e);

				le = le->next;
				if (le) {
					e = (const odict_entry*)le->data;
					if (odict_entry_type(e) != ODICT_STRING) {
						warning("command hangup parameter 3 invalid type\n");
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
				warning("command enqueue: missing parameters\n");
				return nullptr;
			}

			const odict_entry *e = (const odict_entry*)le->data;
			if (odict_entry_type(e) != ODICT_STRING) {
				warning("command enqueue: parameter 1 (call_id) invalid type\n");
				return nullptr;
			}

			const char *call_id = odict_entry_str(e);
			auto sit = Sessions.find(call_id);
			if (sit == Sessions.end()) {
				warning("command enqueue: session %s not found\n", call_id);
				return nullptr;
			}

			Session& session = sit->second;

			le = le->next;
			if (!le) {
				warning("command enqueue: parameter 2 (priority) missing\n");
				return nullptr;
			}

			e = (const odict_entry*)le->data;
			if (odict_entry_type(e) != ODICT_INT) {
				warning("command enqueue: parameter 2 (priority) invalid type\n");
				return nullptr;
			}

			Molecule m;
			m._priority = odict_entry_int(e);

			le = le->next;
			if (!le) {
				warning("command enqueue: parameter 3 (mode) missing\n");
				return nullptr;
			}

			e = (const odict_entry*)le->data;
			if (odict_entry_type(e) != ODICT_INT) {
				warning("command enqueue: parameter 3 (mode) invalid type\n");
				return nullptr;
			}

			m._mode = (mode)odict_entry_int(e);

			int count = 4;
			for (le = le->next; le; le = le->next, ++count) {

				e = (const odict_entry*)le->data;
				if (odict_entry_type(e) != ODICT_OBJECT) {

					if (count == 4 && odict_entry_type(e) == ODICT_OBJECT) {
						// the optional molecule id
						m._id = odict_entry_str(e);
						continue;
					}

					warning("command enqueue: parameter %d (atom) invalid type\n", count);
					return nullptr;
				}

				struct odict *atom = odict_entry_object(e);
				std::string type(odict_string(atom, "type"));

				if (type == "play") {
					const char* filename = odict_string(atom, "filename");
					if (!filename) {
						warning("command enqueue: parameter %d (atom) missing filename\n", count);
						return nullptr;
					}

					m.push_back(std::make_shared<Play>(&session, filename));

					size_t offset = optional_offset(atom);
					if (offset) {
						m.back()->set_offset(offset);
					}
				}
				else if (type == "record") {
					const char* filename = odict_string(atom, "filename");
					if (!filename) {
						warning("command enqueue: parameter %d (atom) missing filename", count);
						return nullptr;
					}

					uint64_t max_silence = 1000;
					odict_get_number(atom, &max_silence, "max_silence");

					uint64_t max_length = 120000;
					odict_get_number(atom, &max_length, "max_length");

					bool dtmf_stop = false;
					odict_get_boolean(atom, &dtmf_stop, "dtmf_stop");

					m.push_back(std::make_shared<Record>(&session, filename, max_silence, max_length, dtmf_stop));
				}
			}

			session._queue.enqueue(m);

			return create_response("enqueue", token, 0);
		}

		return nullptr;
	}

	int villa_status(struct re_printf *pf, void *arg)
	{
		(void)pf;
		(void)arg;

		return 0;
	}
}