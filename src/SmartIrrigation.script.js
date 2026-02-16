function SIR_NoOp(input, output, context) {
}

function SIR_EnsureMaxWeek(input, output, context) {
    if (input.MaxWeek < input.MinWeek) {
        output.MaxWeek = input.MinWeek;
    }
}

function SIR_EnsureTimeWindowEnd(input, output, context) {
    if (input.End == input.Start) {
        var next = input.Start + 1;
        output.End = next > 1439 ? 1439 : next;
    }
}

function SIR_SetPlantDefaults(input, output, context) {
    switch (input.PlantType) {
        case 0: // Rasen
            output.ETFactor = 100;
            output.Interception = 5;
            break;
        case 1: // Rasen (Schatten)
            output.ETFactor = 80;
            output.Interception = 5;
            break;
        case 2: // Gemuese (niedrig wachsend)
            output.ETFactor = 90;
            output.Interception = 10;
            break;
        case 3: // Gemuese (hoch/dicht)
            output.ETFactor = 110;
            output.Interception = 20;
            break;
        case 4: // Blumenbeet
            output.ETFactor = 80;
            output.Interception = 15;
            break;
        case 5: // Hecke
            output.ETFactor = 70;
            output.Interception = 25;
            break;
        case 6: // Buesche
            output.ETFactor = 60;
            output.Interception = 30;
            break;
        case 7: // Kuebelpflanze
            output.ETFactor = 120;
            output.Interception = 0;
            break;
        case 8: // Baum (jung)
            output.ETFactor = 100;
            output.Interception = 20;
            break;
        case 9: // Baum (etabliert)
            output.ETFactor = 50;
            output.Interception = 40;
            break;
        case 10: // Benutzerdefiniert
        default:
            break;
    }
}
