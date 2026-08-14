/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

namespace rts
{
class Task
{
public:
	virtual ~Task();
	virtual void execute() = 0;

protected:
	Task();

private:
	Task(const Task &);
	Task &operator=(const Task &);
};

class TaskRuntime
{
public:
	TaskRuntime();
	~TaskRuntime();

	bool start(unsigned workerCount, unsigned queueCapacity);

	// Ownership transfers only when this method returns true. Rejected tasks remain caller-owned.
	bool trySubmit(Task *task);
	// Ownership of every task transfers only when this method returns true. Rejected tasks remain caller-owned.
	bool trySubmitBatch(Task *const *tasks, unsigned taskCount);
	// Removes a task that has not started. Ownership transfers back to the caller only on success.
	bool tryTake(Task *task);

	void waitUntilIdle();
	void shutdown();

	bool isRunning() const;
	unsigned workerCount() const;
	unsigned pendingTaskCount() const;

private:
	TaskRuntime(const TaskRuntime &);
	TaskRuntime &operator=(const TaskRuntime &);

	struct State;
	State *m_state;
};
}
