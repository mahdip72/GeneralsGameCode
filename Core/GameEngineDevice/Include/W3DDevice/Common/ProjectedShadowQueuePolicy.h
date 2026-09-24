#pragma once

enum ProjectedShadowQueueAttemptResult
{
	PROJECTED_SHADOW_QUEUE_RETRY_SERIAL = -1,
	PROJECTED_SHADOW_QUEUE_COMPLETED = 1
};

/* Only a completed parallel queue attempt owns the decal. Any failure must
 * fall through to the legacy serial path. */
inline bool ProjectedShadowQueueAttemptCompleted(int result)
{
	return result == PROJECTED_SHADOW_QUEUE_COMPLETED;
}
