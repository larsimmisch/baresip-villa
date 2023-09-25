#include <stdlib.h>
#include <string.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include <string>
#include <regex>
#include <chrono>

#ifndef _VILLA_H_
#define _VILLA_H_

enum { PTIME = 40 };

const int max_priority = 5;

enum mode {
	m_discard = 1,
	m_pause = 2,
	m_mute = 4,
	m_restart = 8,
	m_dont_interrupt = 16,
	m_loop = 32,
	m_last = 32,
};

class Play {

public:

	Play() {}
	Play(const std::string& filename) { set_filename(filename); };
	~Play() { stop(); }

	void stop() { _play = (struct play*)mem_deref(_play); }

	// returns the size in ms or 0 in case of an error
	size_t set_filename(const std::string& filename);
	const std::string& filename() const { return _filename; }

	void set_offset(size_t offset) { _offset = offset; }
	size_t offset() const { return _offset; }

	size_t length() const { return _length; }

	struct play *_play = nullptr;

protected:

	std::string _filename;
	size_t _length = 0; // length in ms
	size_t _offset = 0; // offset in ms
};

class Record {

public:

	Record() {}
	Record(const std::string& filename) : _filename(filename) {}
	~Record() { stop(); }

	void stop() { _rec = (struct ausrc_st*)mem_deref(_rec); }

	void set_filename(const std::string& filename) { _filename = filename; }
	const std::string& filename() const { return _filename; }

	void set_max_silence(int max_silence) { _max_silence = max_silence; }
	size_t max_silence() const { return _max_silence; }

	size_t length() const { return _length; }

	struct ausrc_st* _rec = nullptr;

protected:

	std::string _filename;
	int _max_silence = 1000;
	size_t _length = 0;
};

class DTMF {

public:

	DTMF() {}
	DTMF(const std::string& dtmf) : _dtmf(dtmf) {}

	char current() const { return _dtmf[_pos]; }
	size_t operator++() { return  ++_pos; }
	size_t size() const { return _dtmf.size(); }
	void reset() { _pos = 0; }

	void set_dtmf(const std::string& dtmf) { _dtmf = dtmf; }
	const std::string& dtmf() const { return _dtmf; }

	int inter_digit_delay() const { return _inter_digit_delay; }
	void set_inter_digit_delay(int inter_digit_delay) { _inter_digit_delay = inter_digit_delay; }

	void set_offset(size_t offset) {}

	size_t length() const { return _length; }

	struct play *_play = nullptr;

protected:

	std::string _dtmf;
	std::string _lengths;
	int _inter_digit_delay = 100;
	size_t _pos = 0;
	size_t _length = 0;
};

using Atom = std::variant<Play, Record, DTMF>;

struct VQueue;
struct Molecule {
	std::vector<Atom> atoms;
	size_t time_started = 0;
	size_t time_stopped = 0;
	size_t position = 0;
	size_t current = 0;
	int priority = 0;
	mode mode;
	VQueue *_queue;

	size_t length(int start = 0, int end = -1) const;
	void set_position(size_t position_ms);
	// return a description of the Molecule
	std::string desc() const;
};

struct Session;
struct VQueue {

	VQueue(Session *session = nullptr) : _session(session) {}

	void discard(Molecule* m);
	std::vector<Molecule>::iterator next();
	std::vector<Molecule>::iterator end() { return _molecules[0].end(); }

	int schedule(Molecule* stopped);

	int enqueue(const Molecule& m, void* arg);
	int enqueue(const char* mdesc, void* arg);

	std::vector<Molecule> _molecules[max_priority];
	int _current_id;
	Session *_session;
};

struct Session {

	Session(struct call* call, struct json_tcp *_jt);
	Session(const Session& other) = delete;
	Session(Session&& other) {
		_id = std::move(other._id);
		_dtmf = std::move(other._dtmf);
		_dtmf_start = std::move(other._dtmf_start);

		_call = other._call;
		other._call = nullptr;

		_jt = other._jt;
		other._jt = nullptr;

		other._queue._session = this;
		_queue = std::move(other._queue);
		_queue._session = this;
	}

	virtual ~Session();

	virtual void dtmf(char key);
	virtual void hangup(int16_t scode = 200, const char* reason = "BYE");

	std::string _id;
	std::string _dtmf;
	std::chrono::time_point<std::chrono::system_clock> _dtmf_start;
	struct call *_call;
	struct json_tcp *_jt;
	VQueue _queue;
};

#endif // #define _VILLA_H_