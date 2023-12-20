import arrow
import streamlit as st
from st_supabase_connection import SupabaseConnection
import altair as alt
import pandas as pd
import statistics

st.set_page_config(
    page_title="Smart Office Monitoring Dashboard",
    page_icon="🏢",
    layout="wide",
)

conn = st.connection("supabase", type=SupabaseConnection)
measurements = conn.query("*", table="measurements", ttl=0).execute()
# with 0 I disable caching, we want this realtime

st.title("Smart Office Monitoring Dashboard")
st.write("To always safely know the state of your office.")


def get_temperature_chart():
    temperature_data = pd.DataFrame(
        data={
            "measured_at": list(
                map(
                    lambda elem: arrow.get(elem["measured_at"]).datetime,
                    measurements.data,
                )
            ),
            "temp": list(map(lambda elem: elem["temperature"], measurements.data)),
        }
    )

    hover = alt.selection_point(
        fields=["measured_at"],
        nearest=True,
        on="mouseover",
        empty=False,
    )
    lines = (
        alt.Chart(temperature_data, height=500, title="Temperature Monitoring")
        .mark_line()
        .encode(
            x=alt.X("measured_at", title="Measurement Time"),
            y=alt.Y(
                "temp",
                title="Temperature (°C)",
                scale=alt.Scale(domain=[10, 30]),
            ),
        )
    )
    points = lines.transform_filter(hover).mark_circle(size=65)
    tooltips = (
        alt.Chart(temperature_data)
        .mark_rule()
        .encode(
            x="measured_at",
            y="temp",
            opacity=alt.condition(hover, alt.value(0.3), alt.value(0)),
            tooltip=[
                alt.Tooltip(
                    "measured_at", title="Measurement Time", format="%Y-%m-%d %H:%M:%S"
                ),
                alt.Tooltip("temp", title="Temperature (°C)"),
            ],
        )
        .add_params(hover)
    )

    return (lines + points + tooltips).interactive()


def get_humidity_chart():
    humidity_data = pd.DataFrame(
        data={
            "measured_at": list(
                map(
                    lambda elem: arrow.get(elem["measured_at"]).datetime,
                    measurements.data,
                )
            ),
            "hum": list(map(lambda elem: elem["humidity"], measurements.data)),
        }
    )

    hover = alt.selection_point(
        fields=["measured_at"],
        nearest=True,
        on="mouseover",
        empty=False,
    )
    lines = (
        alt.Chart(humidity_data, height=500, title="Humidity Monitoring")
        .mark_line()
        .encode(
            x=alt.X("measured_at", title="Measurement Time"),
            y=alt.Y(
                "hum",
                title="Humidity (%)",
                scale=alt.Scale(domain=[5, 65]),
            ),
        )
    )
    points = lines.transform_filter(hover).mark_circle(size=65)
    tooltips = (
        alt.Chart(humidity_data)
        .mark_rule()
        .encode(
            x="measured_at",
            y="hum",
            opacity=alt.condition(hover, alt.value(0.3), alt.value(0)),
            tooltip=[
                alt.Tooltip(
                    "measured_at", title="Measurement Time", format="%Y-%m-%d %H:%M:%S"
                ),
                alt.Tooltip("hum", title="Humidity (%)"),
            ],
        )
        .add_params(hover)
    )

    return (lines + points + tooltips).interactive()


def get_light_chart():
    light_data = pd.DataFrame(
        data={
            "measured_at": list(
                map(
                    lambda elem: arrow.get(elem["measured_at"]).datetime,
                    measurements.data,
                )
            ),
            "light": list(map(lambda elem: elem["lightness"], measurements.data)),
        }
    )

    hover = alt.selection_point(
        fields=["measured_at"],
        nearest=True,
        on="mouseover",
        empty=False,
    )
    lines = (
        alt.Chart(light_data, height=500, title="Brightness Monitoring")
        .mark_line()
        .encode(
            x=alt.X("measured_at", title="Measurement Time"),
            y=alt.Y(
                "light",
                title="Brightness (lux)",
                scale=alt.Scale(domain=[1, 500]),
            ),
        )
    )
    points = lines.transform_filter(hover).mark_circle(size=65)
    tooltips = (
        alt.Chart(light_data)
        .mark_rule()
        .encode(
            x="measured_at",
            y="light",
            opacity=alt.condition(hover, alt.value(0.3), alt.value(0)),
            tooltip=[
                alt.Tooltip(
                    "measured_at", title="Measurement Time", format="%Y-%m-%d %H:%M:%S"
                ),
                alt.Tooltip("light", title="Brightness (lux)"),
            ],
        )
        .add_params(hover)
    )

    return (lines + points + tooltips).interactive()


temperature_chart = get_temperature_chart()
humidity_chart = get_humidity_chart()
light_chart = get_light_chart()

with st.container():
    col1, col2 = st.columns(2)

    col1.divider()

    col1.write("#### Possible actions")

    actions_cols = col1.columns(5)
    if actions_cols[0].button("Refresh raw data"):
        st.rerun()
    if actions_cols[1].button("Roll up/down shade", disabled=True):
        pass

    col1.write("#### Overview from last day")

    data_from_last_day = list(
        filter(
            lambda elem: elem["temperature"] is not None
            and elem["humidity"] is not None
            and elem["lightness"] is not None,
            filter(
                lambda elem: arrow.get(elem["measured_at"]).datetime
                > arrow.utcnow().shift(days=-1).datetime,
                measurements.data,
            ),
        )
    )

    overview_cols = col1.columns(3)
    overview_cols[0].metric(
        label="Avg. Temperature (°C)",
        help="Average temperature in the office from last day",
        value=f"""{round(
            statistics.mean(
                list(map(lambda elem: elem['temperature'], data_from_last_day))
            ),
            1,
        )} °C""",
    )

    overview_cols[1].metric(
        label="Avg. Humidity (%)",
        help="Average humidity in the office from last day",
        value=f"""{round(
            statistics.mean(
                list(map(lambda elem: elem['humidity'], data_from_last_day))
            ),
            1,
        )} %""",
    )

    overview_cols[2].metric(
        label="Avg. Brightness (%)",
        help="Average brightness in the office from last day",
        value=f"""{round(
            statistics.mean(
                list(map(lambda elem: elem['lightness'], data_from_last_day))
            ),
            1,
        )} lux""",
    )

    col2.altair_chart(temperature_chart.interactive(), use_container_width=True)

with st.container():
    col1, col2 = st.columns(2)
    col1.altair_chart(humidity_chart.interactive(), use_container_width=True)
    col2.altair_chart(light_chart.interactive(), use_container_width=True)

st.divider()

st.subheader("Raw Data Table")
st.dataframe(data=pd.DataFrame(data=measurements.data), use_container_width=True)
