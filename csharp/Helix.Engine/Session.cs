namespace Helix.Engine;

public interface ISessionTransport
{
	bool Send(ReadOnlySpan<byte> message);
	void Close(CloseReason reason);
}

public sealed class Session
{
	private readonly ISessionTransport _transport;
	public ulong Id { get; }
	public Session(ulong id, ISessionTransport transport)
	{
		if (id == 0) throw new ArgumentException("Session id must be positive.", nameof(id));
		Id = id;
		_transport = transport ?? throw new ArgumentNullException(nameof(transport));
	}
	public bool Send(ReadOnlySpan<byte> message) => _transport.Send(message);
	public void Close(CloseReason reason = CloseReason.Kicked) => _transport.Close(reason);
}
