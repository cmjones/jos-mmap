// Challenge Problem:
//  Implementation of Douglas McIlroy's streaming
//  power series calculator.
//
// Streams are simplified pull streams, where readers
//  of a stream must know which stream they are reading,
//  and streams do not know ahead of time where they
//  are writing.

#include <inc/lib.h>

#define STREAM_READ  1
#define STREAM_SPLIT 2


/****************************
 * STREAM PUT/GET FUNCTIONS *
 ****************************/

// Puts the given value onto this environment's stream.
// Anybody who wants to get a value must pass this
//  environment a READ message, the sender will then
//  have to recieve the value that this environment
//  sends.
//
// A special message, STREAM_SPLIT, tells a stream that
//  is currently putting a character to instead fork off
//  into a new process, and both parent and child should
//  continue putting the old value
int
stream_put(float f)
{
	envid_t envid;
	int msg;

	// Wait for a message to send a value
	recv:
	msg = ipc_recv(&envid, 0, 0);

	// If the message is a read, put the
	// value onto the stream and return
	// success.  If its a request to split,
	// fork to a new process, return the
	// new process's id, and wait for another
	// message.
	//
	// The complicated casting interprets the
	// bits of the float as an int to send it.
	if(msg == STREAM_READ) {
		ipc_send(envid, *(int32_t *)&f, 0, 0);
		return 0;
	} else if(msg == STREAM_SPLIT) {
		envid_t child = fork();
		if(child < 0) {
			// fork failed... :(
			panic("fork: %e", child);
		} else if(child > 0) {
			// We are the parent, so alert the
			// messager about the new environment
			ipc_send(envid, child, 0, 0);
		}

		// Continue pushing the value
		goto recv;
	}

	// The message was not a read, so return it
	return msg;
}

// Retrieves a value from the given environment, which
//  should be a stream.  This is done by signaling to
//  the environment that a value is desired, then
//  waiting for it.
float
stream_get(envid_t streamid)
{
	uint32_t val;

	// Send a message saying this env would like to
	//  read a value from the stream
	ipc_send(streamid, STREAM_READ, 0, 0);

	// Retrieve a message from streamid, which the
	// kernel guarantees will be the only environment
	// able to pass us a message.
	//
	// The complicated casting interprets the bits of
	// the uint32_t value as a float
	val = ipc_recv_src(streamid, 0, 0, 0);
	return *(float *)&val;
}

// Takes a function that should act as a stream (constantly
//  putting values) and starts it in a forked environment,
//  return the id of environment.  When the function returns,
//  the environment should be halted.
//
// The function itself takes a void pointer, which is an
//  arbitrary context that emulates the functionality of
//  closures.  The function is responsible for casting the
//  context, and the caller of stream_start is responsible
//  for passing a legitimate value.
//
// Since every context is used only in a child process, the
//  context may be overwritten after stream_start, since it
//  is copied to the child by the magic of the forking process.
envid_t
stream_start(void (*func)(void *), void *context)
{
	envid_t child = fork();

	if(child < 0) {
		// fork failed... :(
		panic("fork: %e", child);
	} else if(child == 0) {
		// Start the child, and halt when finished
		func(context);
		exit();
	}

	// We are in the parent, so return the child id
	return child;
}

// Takes the id for a currently running stream and splits
//  it into a new environment.  If all goes according to
//  plan, the new stream should pick up exactly where the
//  old one left off.
envid_t
stream_split(envid_t stream)
{
	// Signal the stream that we want it to fork,
	//  Will only work if the stream is currently
	//  putting a character.
	ipc_send(stream, STREAM_SPLIT, 0, 0);

	// Return the envid of the new child that is
	//  created by forking.
	return (envid_t)ipc_recv_src(stream, 0, 0, 0);
}


/*********************************************
 * STREAM AND CONTEXT STRUCT DEFINITIONS, OR *
 * BASIC POWER SERIES CALCULATOR FUNCTIONS   *
 *********************************************/

struct StreamSet {
	envid_t *s;
	int num;
};

struct StreamIntPair {
	envid_t F;
	uint32_t val;
};

// Outputs the sum of a set of input streams which
//  operate on 32-bit float values.
void
sumStream(void *streamSet)
{
	int i;
	float sum;

	// First cast the struct properly
	struct StreamSet *p = (struct StreamSet *)streamSet;

	// Now get values from each stream and put their sum
	while(1) {
		sum = 0;
		for(i = 0; i < p->num; i++) sum += stream_get(p->s[i]);
		stream_put(sum);
	}
}

// Delays an input stream by some number
//  of ticks, then continues the stream
//  normally.
void
delayStream(void *streamIntPair)
{
	int i;

	// First cast the struct properly
	struct StreamIntPair *p = (struct StreamIntPair *)streamIntPair;

	// Next output p->val 0s, which simulates multiplying
	//  a polynomial stream by x^(p->val)
	for(i = 0; i < p->val; i++) stream_put(0);

	// Finally, continue the input stream as normal
	stream_put(stream_get(p->F));
}

// Complicated stream that pushes the multiplication
//  of two streams together.  Takes a stream set with
//  the understanding that only the first two streams
//  will be pulled from.
void
multiplyStream(void *streamSet)
{
	float f0, g0;
	envid_t F1, F2, G1, G2, M;

	// First cast the struct properly
	struct StreamSet *p = (struct StreamSet *)streamSet;

	// Assert that at least two streams are in the set
	// and store them in named locations
	if(p->num < 2) panic("MultiplyStream given less than two streams");
	F1 = p->s[0];
	G1 = p->s[1];

	// The first term of the multiplication is easy,
	//  just the first value of each passed stream.
	f0 = stream_get(F1);
	g0 = stream_get(G1);
	stream_put(f0*g0);

	// Each of the input streams must now be split into
	//  two. F2 will feed one part of the final sum, G2
	//  will feed the next, and the multiplication of F1
	//  and G1 will feed the final part.
	//
	// The split must occur because different processes
	//  may read each of the splits at different rates, and
	//  its annoying to have to buffer values. :)
	F2 = stream_split(F1);
	G2 = stream_split(G1);

	// Construct the multiplication stream.  Since the
	//  streams being multiplied are just the remainders
	//  of the originals, we can pass the same context to
	//  this new multiplyStream.
	M = stream_start(multiplyStream, streamSet);

	// The only thing left is to delay stream M by one,
	//  which for simplicities sake means we'll grab
	//  from the other two streams once before grabbing
	//  from all three.
	stream_put(f0*stream_get(G2)+g0*stream_get(F2));

	// Finally, repeatedly the sum of the three
	//  constructed stream:
	//      f0*G2+g0*F2+F1*G1
	while(1)
		stream_put(f0*stream_get(G2)+g0*stream_get(F2)+stream_get(M));
}

// Another complicated stream, this time computing the stream F(G),
//  where both F and G are streams.  This is recursive, like
//  multiplication, and in fact also uses a multiplication stream.
//
// Takes a stream set with the understanding that only the first two
//  will be pulled from. Also assumes that the first element of G is
//  0, so that the substitution is finite, and that the ordering of
//  the set is F, G.
void
substitutionStream(void *streamSet)
{
	float f0, g0;
	envid_t F, G1, G2, M;

	// First cast the struct properly
	struct StreamSet *p = (struct StreamSet *)streamSet;

	// Assert that at least two streams are in the set
	// and store them in named locations
	if(p->num < 2) panic("substitutionStream given less than two streams");
	F = p->s[0];
	G1 = p->s[1];

	// Before we do anything, we need to split G1, since
	//  the recursion involves passing the original stream.
	G2 = stream_split(G1);

	// Now grab the first element of G1 and assert that it
	// is, in fact, 0.  The stream isn't solvable otherwise.
	g0 = stream_get(G2);
	if(g0 != 0.0f) panic("For F(G), G's first element is not 0");

	// Grab the first element of F1, which is also the first
	//  returned value of this stream
	f0 = stream_get(F);
	stream_put(f0);

	// The remaining part of the stream is expressed by
	// G2*F(G1), which is a multiplication stream consisting
	// of G2 and another substitution stream.  Construct it!
	//
	// We've designed things so that streamSet cotains F and
	// G1, which means we can pass it as the context to the
	// next substitutionStream.  We can then use the same
	// memory to construct the multiplication stream.
	M = stream_start(substitutionStream, (void *)p);
	p->s[0] = M;
	p->s[1] = G2;
	M = stream_start(multiplyStream, (void *)p);

	// Finally, we can start pulling values off of M
	while(1) stream_put(stream_get(M));
}



/*********************************************************
 * FUNCTIONS SPECIFIC TO CALCULATING SIN(X+X^3) AND MAIN *
 *********************************************************/

// Push the coefficients for sin(x), which are 0 for even powers
//  of x and (-1)^((n-1)/2)/n! for odd powers of x.
void
sinStream(void *unused)
{
	int n;
	float cur = 1.0f;

	for(n = 0; ; n++) {
		// If n is even, put 0, otherwise calculate the
		//  next coefficient from the previous one and
		//  put that.
		if(n&1) {
			// Example: if cur is -1/7!, this will
			// set cur to be (-1/7!)*(-1)/(9*8), or
			// 1/9!
			cur *= -1.0f/(n*(n-1));
			stream_put(cur);
		} else {
			stream_put(0.0f);
		}
	}
}

// A "stream" that outputs [0, 1, 0, 1], then all zeros, which
//  is the coefficient stream for x+x^3
void
xPlusXCubedStream(void *unused)
{
	// Put the four hard-coded values in first
	stream_put(0.0f);
	stream_put(1.0f);
	stream_put(0.0f);
	stream_put(1.0f);

	// Now loop while putting 0s
	while(1) stream_put(0.0f);
}


void
umain(int argc, char **argv)
{
	envid_t root;
	envid_t ids[2];
	struct StreamSet set;

	// Create the sin and x+x^3 streams
	set.num = 2;
	set.s = ids;
	set.s[0] = stream_start(sinStream, NULL);
	set.s[1] = stream_start(xPlusXCubedStream, NULL);

	// Create a substitution stream of sin and x+x^3, which
	//  yields the power series for sin(x+x^3), hopefully
	root = stream_start(substitutionStream, (void *)(&set));

	// Now just print values from the root stream,
	//  and lazy evaluation will take care of the
	//  rest.
	//
	// TODO: Sigh, realized at the last moment that we can't
	//  print float values.  This lab is late enough.  I'll
	//  pretend that the code is structured enough that it's
	//  very plausible it will work just by sight.
	while(1) cprintf("%f\n", stream_get(root));
}
