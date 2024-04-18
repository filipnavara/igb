#pragma once

#if DBG
#define DBGPRINT(...) DbgPrint(__VA_ARGS__)
#else
#define DBGPRINT(...) 
#endif

#define LOG_NTSTATUS(Status) DBGPRINT("%s:%d: Status %x\n", __FILE__, __LINE__, Status)

#define LOG_IF_NOT_NT_SUCCESS(Expression, ...) do { \
	NTSTATUS p_status = (Expression); \
	if (!NT_SUCCESS(p_status)) \
	{ \
		LOG_NTSTATUS(p_status); \
	} \
} while(0)

#define GOTO_IF_NOT_NT_SUCCESS(Label, StatusLValue, Expression, ...) do { \
	StatusLValue = (Expression); \
	if (!NT_SUCCESS(StatusLValue)) \
	{ \
		LOG_NTSTATUS(StatusLValue); \
		goto Label; \
	} \
} while (0)

#define GOTO_WITH_INSUFFICIENT_RESOURCES_IF_NULL(Label, StatusLValue, Object) \
	GOTO_IF_NOT_NT_SUCCESS(Label, StatusLValue, (((Object) == NULL) ? STATUS_INSUFFICIENT_RESOURCES : STATUS_SUCCESS))
