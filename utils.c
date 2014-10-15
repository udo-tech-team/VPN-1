#include <gtk/gtk.h>
#include <string.h>
#include <stdlib.h>

void getHex(char *bytes, char *hex, int len)
{
    int i;
    int pos = 0;
    for( i = 0; i < len; i++)
    {
        pos += sprintf(&hex[pos], "%02X ", (unsigned char) bytes[i]);
    }
}

void writeHex(GtkWidget *textView, char *prefixText, char *textAsHex, int length)
{
    char outText[length * 2 + 1];
    GtkTextIter iter;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textView));
    gtk_text_buffer_get_start_iter(buffer, &iter);

    int i;
    int pos = 0;
    for(i = 0; i<length; i++){
        pos += sprintf(&outText[pos], "%02X ", (unsigned char) textAsHex[i]);
    }
    sprintf(&outText[pos], "\n");
    gtk_text_buffer_insert(buffer, &iter, prefixText, strlen(prefixText));
    gtk_text_buffer_insert(buffer, &iter, outText, strlen(outText));
}

void writeLine(GtkWidget *textView, const char *text)
{
    GtkTextIter iter;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textView));
    gtk_text_buffer_get_start_iter(buffer, &iter);
    gtk_text_buffer_insert(buffer, &iter, text, strlen(text));
    gtk_text_buffer_insert(buffer, &iter, "\n", strlen("\n"));
}