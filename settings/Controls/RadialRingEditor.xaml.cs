using System;
using Keychain.Settings.Models;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;

namespace Keychain.Settings.Controls;

public sealed partial class RadialRingEditor : UserControl
{
    private const double CanvasSize = 360.0;
    private const double OuterRadius = 170.0;
    private const double InnerRadius = 74.0;
    private const double SectorGapDeg = 2.6;

    private ConfigDocument? _doc;
    private RadialRing? _ring;
    private int _selectedIndex = -1;

    public event Action<int>? SectorSelected;

    public RadialRingEditor()
    {
        InitializeComponent();
    }

    public void Bind(ConfigDocument doc, RadialRing ring, int selectedSector = -1)
    {
        _doc = doc;
        _ring = ring;
        _selectedIndex = Math.Clamp(selectedSector, -1, ring.SectorCount - 1);
        Rebuild();
    }

    public void SelectSector(int index)
    {
        if (_ring is null) return;
        _selectedIndex = Math.Clamp(index, -1, _ring.SectorCount - 1);
        Rebuild();
    }

    private void Rebuild()
    {
        RingCanvas.Children.Clear();
        if (_doc is null || _ring is null) return;

        RingNameText.Text = $"{_ring.Name} ({_ring.SectorCount} sectors) - tap a sector, then assign an action below.";
        var stepDeg = 360.0 / _ring.SectorCount;
        var cx = CanvasSize / 2.0;
        var cy = CanvasSize / 2.0;

        for (int i = 0; i < _ring.SectorCount; i++)
        {
            double a0 = -90.0 + i * stepDeg + SectorGapDeg * 0.5;
            double a1 = -90.0 + (i + 1) * stepDeg - SectorGapDeg * 0.5;
            if (a1 <= a0) a1 = a0 + 0.5;

            var sector = BuildSectorPath(cx, cy, a0, a1);

            string actionId = i < _ring.SectorActionIds.Count ? _ring.SectorActionIds[i] : "";
            var action = string.IsNullOrEmpty(actionId) ? null : _doc.FindAction(actionId);
            string label = action?.Label ?? "None";
            bool isChildLink = action?.Type == "OpenChildRing";
            bool isDisabled = action is { Enabled: false };

            sector.Fill = i == _selectedIndex
                ? (Brush)Resources["SectorSelectedBrush"]
                : isDisabled
                    ? (Brush)Resources["SectorDisabledBrush"]
                    : isChildLink
                        ? (Brush)Resources["SectorChildBrush"]
                        : (Brush)Resources["SectorNormalBrush"];
            sector.Stroke = (Brush)Resources["SectorStrokeBrush"];
            sector.StrokeThickness = 1;
            sector.Tag = i;
            sector.UseLayoutRounding = true;

            AutomationProperties.SetName(sector, $"Sector {i + 1} of {_ring.SectorCount}: {label}");
            sector.Tapped += OnSectorTapped;
            RingCanvas.Children.Add(sector);

            var icon = new TextBlock
            {
                Text = action?.IconGlyph ?? (i + 1).ToString(),
                FontSize = 16,
                FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
                Foreground = i == _selectedIndex
                    ? new SolidColorBrush(Microsoft.UI.Colors.White)
                    : (Brush)Application.Current.Resources["TextFillColorPrimaryBrush"],
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
            };
            double midDeg = -90.0 + (i + 0.5) * stepDeg;
            double midR = (InnerRadius + OuterRadius) * 0.5;
            double rad = midDeg * Math.PI / 180.0;
            double textX = cx + midR * Math.Cos(rad);
            double textY = cy + midR * Math.Sin(rad);
            Canvas.SetLeft(icon, textX - 18);
            Canvas.SetTop(icon, textY - 12);
            icon.IsHitTestVisible = false;
            RingCanvas.Children.Add(icon);
        }

        var hub = new Ellipse
        {
            Width = InnerRadius * 1.55,
            Height = InnerRadius * 1.55,
            Fill = (Brush)Resources["HubBrush"],
            Stroke = (Brush)Resources["SectorStrokeBrush"],
            StrokeThickness = 1,
            IsHitTestVisible = false,
        };
        Canvas.SetLeft(hub, cx - hub.Width / 2);
        Canvas.SetTop(hub, cy - hub.Height / 2);
        RingCanvas.Children.Add(hub);

        var hubText = new TextBlock
        {
            Text = _ring.Name,
            FontSize = 12,
            TextAlignment = TextAlignment.Center,
            Foreground = (Brush)Application.Current.Resources["TextFillColorPrimaryBrush"],
            Width = InnerRadius * 1.3,
            IsHitTestVisible = false,
        };
        Canvas.SetLeft(hubText, cx - hubText.Width / 2);
        Canvas.SetTop(hubText, cy - 10);
        RingCanvas.Children.Add(hubText);
    }

    private Microsoft.UI.Xaml.Shapes.Path BuildSectorPath(double cx, double cy, double a0Deg, double a1Deg)
    {
        static Windows.Foundation.Point Polar(double x, double y, double r, double deg)
        {
            double rad = deg * Math.PI / 180.0;
            return new Windows.Foundation.Point(x + r * Math.Cos(rad), y + r * Math.Sin(rad));
        }

        double spanDeg = a1Deg - a0Deg;
        var p0 = Polar(cx, cy, OuterRadius, a0Deg);
        var p1 = Polar(cx, cy, OuterRadius, a1Deg);
        var p2 = Polar(cx, cy, InnerRadius, a1Deg);
        var p3 = Polar(cx, cy, InnerRadius, a0Deg);

        var figure = new PathFigure { StartPoint = p0, IsClosed = true };
        figure.Segments.Add(new ArcSegment
        {
            Point = p1,
            Size = new Windows.Foundation.Size(OuterRadius, OuterRadius),
            SweepDirection = SweepDirection.Clockwise,
            IsLargeArc = spanDeg > 180.0,
        });
        figure.Segments.Add(new LineSegment { Point = p2 });
        figure.Segments.Add(new ArcSegment
        {
            Point = p3,
            Size = new Windows.Foundation.Size(InnerRadius, InnerRadius),
            SweepDirection = SweepDirection.Counterclockwise,
            IsLargeArc = spanDeg > 180.0,
        });

        var geometry = new PathGeometry();
        geometry.Figures.Add(figure);
        return new Microsoft.UI.Xaml.Shapes.Path { Data = geometry };
    }

    private void OnSectorTapped(object sender, TappedRoutedEventArgs e)
    {
        if (sender is not Microsoft.UI.Xaml.Shapes.Path path || path.Tag is not int index) return;
        _selectedIndex = index;
        Rebuild();
        SectorSelected?.Invoke(index);
    }
}
