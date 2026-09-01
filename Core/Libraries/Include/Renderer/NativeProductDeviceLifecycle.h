#ifndef RTS_RENDERER_NATIVEPRODUCTDEVICELIFECYCLE_H
#define RTS_RENDERER_NATIVEPRODUCTDEVICELIFECYCLE_H

namespace rts
{
namespace render
{

// Small, platform-neutral owner for the native product device lifecycle.  The
// callbacks keep window-system and graphics API types at the integration seam,
// while tests can exercise the exact create/resize/shutdown ordering headlessly.
struct NativeProductDeviceOperations
{
	NativeProductDeviceOperations() : context(0), initialize(0), prepareResize(0),
		resize(0), shutdown(0) {}

	void *context;
	bool (*initialize)(void *context, unsigned int width,
		unsigned int height, bool enableVsync);
	bool (*prepareResize)(void *context);
	bool (*resize)(void *context, unsigned int width, unsigned int height);
	void (*shutdown)(void *context);
};

class NativeProductDeviceLifecycle
{
public:
	enum State
	{
		UNBOUND,
		READY,
		ACTIVE,
		LOST
	};

	NativeProductDeviceLifecycle() : m_state(UNBOUND) {}

	bool bind(const NativeProductDeviceOperations &operations)
	{
		if (m_state != UNBOUND || operations.context == 0 ||
			operations.initialize == 0 || operations.prepareResize == 0 ||
			operations.resize == 0 || operations.shutdown == 0)
		{
			return false;
		}
		m_operations = operations;
		m_state = READY;
		return true;
	}

	bool create(unsigned int width, unsigned int height, bool enableVsync)
	{
		if (m_state != READY || width == 0 || height == 0)
		{
			return false;
		}
		if (!m_operations.initialize(m_operations.context, width, height,
			enableVsync))
		{
			return false;
		}
		m_state = ACTIVE;
		return true;
	}

	bool reset(unsigned int width, unsigned int height)
	{
		if (m_state != ACTIVE || width == 0 || height == 0)
		{
			return false;
		}
		if (!m_operations.prepareResize(m_operations.context) ||
			!m_operations.resize(m_operations.context, width, height))
		{
			m_state = LOST;
			return false;
		}
		return true;
	}

	void shutdown()
	{
		if (m_state == ACTIVE || m_state == LOST)
		{
			m_operations.shutdown(m_operations.context);
		}
		m_operations = NativeProductDeviceOperations();
		m_state = UNBOUND;
	}

	State state() const { return m_state; }
	bool isActive() const { return m_state == ACTIVE; }
	bool ownsDeviceResources() const
	{
		return m_state == ACTIVE || m_state == LOST;
	}

private:
	NativeProductDeviceLifecycle(const NativeProductDeviceLifecycle &);
	NativeProductDeviceLifecycle &operator=(
		const NativeProductDeviceLifecycle &);

	NativeProductDeviceOperations m_operations;
	State m_state;
};

}
}

#endif
