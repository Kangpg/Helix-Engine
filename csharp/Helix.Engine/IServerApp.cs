namespace Helix.Engine;

public enum CloseReason { Normal, PeerClosed, IdleTimeout, IoError, ProtocolError, ServerShutdown, Kicked }

// 동일 세션 콜백은 순차 실행. 다른 세션 콜백은 동시에 실행될 수 있다.
public interface IHelixApp
{
	void OnStart(HelixServer server) { }
	void OnConnect(Session session) { }
	void OnMessage(Session session, ReadOnlySpan<byte> message);
	void OnDisconnect(Session session, CloseReason reason) { }
	void OnStop() { }
}

public delegate void MessageHandler(Session session, ReadOnlySpan<byte> message);

public sealed class DelegateServerApp(MessageHandler onMessage) : IHelixApp
{
	private readonly MessageHandler _onMessage = onMessage ?? throw new ArgumentNullException(nameof(onMessage));
	public Action<Session>? Connected { get; init; }
	public Action<Session, CloseReason>? Disconnected { get; init; }
	public void OnConnect(Session session) => Connected?.Invoke(session);
	public void OnMessage(Session session, ReadOnlySpan<byte> message) => _onMessage(session, message);
	public void OnDisconnect(Session session, CloseReason reason) => Disconnected?.Invoke(session, reason);
}
