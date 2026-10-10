// EntityPlayerSP
public class bln extends wn {
    public int sent;
    public void e(String text) { sent++; System.out.println("sent: " + text); }      // sendChatMessage
    public void b(eu message) { System.out.println("chat: " + message.c()); } // addChatMessage
}
