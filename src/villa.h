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

template<typename X>
struct deleter {
	void operator()(X* x) const {
		x = (X*)mem_deref(x);
	}
};

// Wrapping raw pointers

using play_ptr = std::unique_ptr<struct play, deleter<struct play> >;
using ausrc_st_ptr = std::unique_ptr<struct ausrc_st, deleter<struct ausrc_st> >;

enum mode {
	m_discard = 1,
	m_pause = 2,
	m_mute = 4,
	m_restart = 8,
	m_dont_interrupt = 16,
	m_loop = 32,
	m_last = 32,
};


struct Session;
struct Molecule;

struct AudioOp {

	virtual int start(Molecule *) = 0;
	virtual void stop() = 0;

	// length in ms
	virtual size_t length() const  = 0;

	virtual void set_offset(size_t) {}
	virtual size_t offset() const { return 0; }

	virtual bool done() { return true; }

	virtual std::string desc() = 0;
};

using AudioOpPtr = std::shared_ptr<AudioOp>;

class Play : public AudioOp {

public:

	Play(const std::string& filename) { set_filename(filename); };
	virtual ~Play() {}

	virtual int start(Molecule *);
	virtual void stop() { if (_audio) { audio_set_source(_audio, nullptr, nullptr); _audio = nullptr; } }

	size_t set_filename(const std::string& filename);
	const std::string& filename() const { return _filename; }

	virtual void set_offset(size_t offset) { _offset = offset; }
	virtual size_t offset() const { return _offset; }

	virtual size_t length() const { return _length; }

	virtual std::string desc() { return std::string("play ") + _filename; }

protected:

	struct audio *_audio = nullptr;
	std::string _filename;
	size_t _length = 0; // length in ms
	size_t _offset = 0; // offset in ms
};

class Record : public AudioOp {

public:

	Record(const std::string& filename) : _filename(filename) {}
	virtual ~Record() { stop(); }

	virtual int start(Molecule *);
	virtual void stop() {_rec = (struct ausrc_st*)mem_deref(_rec); }

	void set_filename(const std::string& filename) { _filename = filename; }
	const std::string& filename() const { return _filename; }

	void set_max_silence(int max_silence) { _max_silence = max_silence; }
	size_t max_silence() const { return _max_silence; }

	virtual size_t length() const { return _length; }

	virtual std::string desc() { return std::string("record ") + _filename; }

protected:

	ausrc_st *_rec;
	std::string _filename;
	int _max_silence = 1000;
	size_t _length = 0;
};

class DTMF : public AudioOp {

public:

	DTMF(const std::string& dtmf) : _dtmf(dtmf) {}
	virtual ~DTMF() { stop(); }

	virtual int start(Molecule*);
	virtual void stop() { _play = (struct play*)mem_deref(_play); }

	char current() const { return _dtmf[_pos]; }
	size_t operator++() { return  ++_pos; }
	size_t size() const { return _dtmf.size(); }
	void reset() { _pos = 0; }

	virtual bool done() { return _pos >= size(); }

	void set_dtmf(const std::string& dtmf) { _dtmf = dtmf; }
	const std::string& dtmf() const { return _dtmf; }

	int inter_digit_delay() const { return _inter_digit_delay; }
	void set_inter_digit_delay(int inter_digit_delay) { _inter_digit_delay = inter_digit_delay; }

	virtual size_t length() const { return _length; }

	virtual std::string desc() { return std::string("DTMF ") + _dtmf; }

protected:

	struct play *_play;
	std::string _dtmf;
	int _inter_digit_delay = 100;
	size_t _pos = 0;
	size_t _length = 0;
};

struct Molecule {

	Molecule(Session *session) : _session(session) {}

	void push_back(const AudioOpPtr &a) { _atoms.push_back(a); }

	AudioOpPtr &back() { return _atoms.back(); }
	size_t size() { return _atoms.size(); }

	size_t length(int start = 0, int end = -1) const;
	void set_position(size_t position_ms);
	// return a description of the Molecule
	std::string desc() const;

	std::vector<AudioOpPtr> _atoms;
	size_t _current = 0;
	size_t _time_started = 0;
	size_t _time_stopped = 0;
	size_t _position = 0;
	int _priority = 0;
	mode _mode;
	Session *_session;
};
struct VQueue {

	VQueue(Session *session = nullptr) : _session(session) {
		_molecules.resize(max_priority + 1);
	}

	void discard(Molecule* m);
	std::vector<Molecule>::iterator next();
	std::vector<Molecule>::iterator end() { return _molecules[0].end(); }

	int schedule(Molecule* stopped);

	int enqueue(const Molecule& m);

	std::vector<std::vector<Molecule> > _molecules;
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
	struct player *_player = nullptr;
	VQueue _queue;
};

#endif // #define _VILLA_H_