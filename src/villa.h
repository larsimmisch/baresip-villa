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

	AudioOp(Session *session) : _session(session), _stopped(false) {}

	virtual int start() = 0;
	virtual void stop() = 0;

	// length in ms
	virtual size_t length() const  = 0;

	virtual void set_offset(size_t) {}
	virtual size_t offset() const { return 0; }

	virtual bool done() { return true; }

	virtual void event_vad(Session*, bool) {}
	virtual void event_dtmf(Session*, char, bool) {}

	virtual std::string desc() = 0;

	Session *_session;
	bool _stopped;
};

using AudioOpPtr = std::shared_ptr<AudioOp>;

class Play : public AudioOp {

public:

	Play(Session *session, const std::string& filename) : AudioOp(session) { set_filename(filename); };
	virtual ~Play() {}

	virtual int start();
	virtual void stop();
	size_t set_filename(const std::string& filename);
	const std::string& filename() const { return _filename; }

	virtual void set_offset(size_t offset) { _offset = offset; }
	virtual size_t offset() const { return _offset; }

	virtual size_t length() const;

	virtual std::string desc() { return std::string("play ") + _filename; }

protected:

	struct audio *_audio = nullptr;
	std::string _filename;
	mutable size_t _length = 0; // length in ms
	size_t _offset = 0; // offset in ms
};

class Record;



class Record : public AudioOp {

public:

	enum timer_id_t {
		timer_max_silence,
		timer_max_length
	};

	struct TimerId {
		TimerId (Record *r, timer_id_t t) : record(r), timer_id(t) {}

		Record* record;
		timer_id_t timer_id;
	};

	Record(Session *session, const std::string& filename, int max_silence=1000,
		int max_length=120000, bool dtmf_stop=false)
			: AudioOp(session), _filename(filename), _max_silence(max_silence),
			_max_length(max_length), _dtmf_stop(dtmf_stop),
			_timer_max_silence_id(this, timer_max_silence), _timer_max_length_id(this, timer_max_length) {
		tmr_init(&_tmr_max_length);
		tmr_init(&_tmr_max_silence);
	}
	virtual ~Record() { stop(); }

	virtual int start();
	virtual void stop();

	virtual void event_vad(Session *, bool vad);
	virtual void event_dtmf(Session*, char, bool);

	void set_filename(const std::string& filename) { _filename = filename; }
	const std::string& filename() const { return _filename; }

	void set_max_silence(int max_silence) { _max_silence = max_silence; }
	size_t max_silence() const { return _max_silence; }

	virtual size_t length() const { return _length; }

	virtual std::string desc() { return std::string("record ") + _filename; }

protected:

	struct audio *_audio = nullptr;
	size_t _last_vad_tstamp;
	struct tmr _tmr_max_length;
	struct tmr _tmr_max_silence;
	std::string _filename;
	int _max_silence;
	int _max_length;
	size_t _length = 0;
	bool _dtmf_stop;
	TimerId _timer_max_silence_id;
	TimerId _timer_max_length_id;
};


struct Molecule {

	void push_back(const AudioOpPtr &a) { _atoms.push_back(a); }

	AudioOpPtr &back() { return _atoms.back(); }
	size_t size() const { return _atoms.size(); }
	AudioOpPtr &current();
	bool is_active() const { return _current < size(); }

	size_t length(int start = 0, int end = -1) const;
	void set_position(size_t position_ms);
	// return a description of the Molecule
	std::string desc() const;

	std::vector<AudioOpPtr> _atoms;
	size_t _current = 0;
	size_t _time_started = 0;
	size_t _time_stopped = 0;
	int _priority = 0;
	mode _mode;
	std::string _id;
};

struct VQueue {

	enum reason {
		sched_start,
		sched_interrupt,
		sched_dtmf,
		sched_end_of_file
	};

	VQueue(Session *session = nullptr) : _session(session) {
		_molecules.resize(max_priority + 1);
	}

	void discard(Molecule* m);
	std::vector<Molecule>::iterator next();
	std::vector<Molecule>::iterator end() { return _molecules[0].end(); }

	int schedule(reason);

	int enqueue(const Molecule& m);

	std::vector<std::vector<Molecule> > _molecules;
	Molecule *_active = nullptr;
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
	virtual void molecule_done(const Molecule &m) const;

	std::string _id;
	std::string _dtmf;
	std::chrono::time_point<std::chrono::system_clock> _dtmf_start;
	struct call *_call;
	struct json_tcp *_jt;
	VQueue _queue;
	bool _vad;
};

#endif // #define _VILLA_H_