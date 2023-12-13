CREATE TABLE user (
	id text not null,
	password text,
	email text,
	flags text,
	time_created text
);

CREATE TABLE caller_ids (
	id text not null,
	caller_id text
);

CREATE TABLE direct_messages (
	sender text not null,
	receiver text not null,
	path text not null,
	time_created text
);

CREATE TABLE messages (
	location text not null,
	sender text not null,
	time_created text not null
);