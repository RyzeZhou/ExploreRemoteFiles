using System.Windows;
using RemoteFsClient.Services;

namespace RemoteFsClient;

/// <summary>用户在票据决策框里选了什么。Secondary 为「不要第二个选项」时不会出现。</summary>
public enum TicketChoice { Primary, Secondary, Cancel }

/// <summary>
/// 票据相关的决策框。**按钮文字必须就是选项本身** —— 之前用 MessageBox 的
/// 「是/否/取消」，按钮与正文里描述的「迁移 / 重新下载」对不上，用户没法确定哪个是哪个。
/// </summary>
public partial class TicketChoiceWindow : Window
{
    public TicketChoice Choice { get; private set; } = TicketChoice.Cancel;

    public TicketChoiceWindow(string title, string message, string primary,
                              string? secondary = null, string? cancel = null)
    {
        InitializeComponent();
        Title = title;
        MessageText.Text = message;
        PrimaryButton.Content = primary;
        if (string.IsNullOrEmpty(secondary))
        {
            SecondaryButton.Visibility = Visibility.Collapsed;
        }
        else
        {
            SecondaryButton.Content = secondary;
            SecondaryButton.Visibility = Visibility.Visible;
        }
        CancelButton.Content = string.IsNullOrEmpty(cancel) ? Ui.T("Cancel") : cancel;
        Loaded += (_, _) => { Activate(); PrimaryButton.Focus(); };
    }

    private void OnPrimary(object sender, RoutedEventArgs e) { Choice = TicketChoice.Primary; DialogResult = true; }
    private void OnSecondary(object sender, RoutedEventArgs e) { Choice = TicketChoice.Secondary; DialogResult = true; }
    private void OnCancel(object sender, RoutedEventArgs e) { Choice = TicketChoice.Cancel; DialogResult = false; }
}
