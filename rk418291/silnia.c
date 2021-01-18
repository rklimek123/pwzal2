#include <stdio.h>
#include "cacti.h"

typedef unsigned long long num_t;

#define MSG_FACTORIZE 1
#define MSG_SEND 2

message_t message_spawn(role_t* role) {
	message_t msg;
	msg.message_type = MSG_SPAWN;
	msg.nbytes = sizeof(role->nprompts) + sizeof(role->prompts);

	for (size_t i = 0; i < role->nprompts; ++i) {
		msg.nbytes += sizeof(*(role->prompts + i));
	}

	msg.data = role;

	return msg;
}

message_t message_godie() {
	message_t msg;
	msg.message_type = MSG_GODIE;
	return msg;
}

message_t message_factorize(size_t nbytes, void* data) {
	message_t msg;
	msg.message_type = MSG_FACTORIZE;
	msg.nbytes = nbytes;
	msg.data = data;
	return msg;
}

message_t message_send(size_t nbytes, void* data) {
	message_t msg;
	msg.message_type = MSG_SEND;
	msg.nbytes = nbytes;
	msg.data = data;
	return msg;
}


void hello(void** stateptr, size_t nbytes, void* data) {}

void factorize(void **stateptr, size_t nbytes, void *data) {
	num_t* my_data = (num_t*)data;
	num_t n = my_data[0];
	num_t k = my_data[1];
	num_t k_factorial = my_data[2];

	if (k == n) {
		my_data[0] = k_factorial;
		
	}
	else {
		my_data[1] = k + 1;
		my_data[2] = k_factorial * (k + 1);
		
		if (send_message(actor_id_self(), message_spawn(my_data[3])) != 0)
			exit(-2);

		if (send_message(actor_id_self(), message_send(nbytes, data)) != 0)
			exit(-2);
	}

	if (send_message(actor_id_self(), message_godie()) != 0)
		exit(-2);
}

void send(void** stateptr, size_t nbytes, void* data) {
	if (send_message(actor_id_self() + 1, message_factorize(nbytes, data)) != 0)
		exit(-2);
}


int main() {
	long long number;
	scanf("%lld", &number);

	if (number <= 0) {
		printf("Calculated without using actors' system: %d\n", 1);
		return 0;
	}

	role_t* role;
	role->nprompts = 3;
	role->prompts = (act_t*)malloc(sizeof(act_t) * role->nprompts);

	*(role->prompts) = hello;
	*(role->prompts + sizeof(act_t*)) = factorize;
	*(role->prompts + sizeof(act_t*) * 2) = send;

	num_t n = (num_t)number;
	num_t k = 1;
	num_t k_factorial = 1;

	size_t nbytes = sizeof(role_t) + 3 * sizeof(num_t);
	void* data[4];
	data[0] = &n;
	data[1] = &k;
	data[2] = &k_factorial;
	data[3] = role;

	actor_id_t a;

	if (actor_system_create(&a, role) != 0)
		exit(-1);

	int check = send_message(a, message_factorize(nbytes, (void*)data));

	if (check != 0)
		exit(check);

	actor_system_join(a);
	printf("%llu\n", (num_t)data[0]);

	return 0;
}
